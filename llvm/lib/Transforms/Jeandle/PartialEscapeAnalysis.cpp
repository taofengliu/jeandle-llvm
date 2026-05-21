//===-- PartialEscapeAnalysis.cpp - PEA (analysis pass) -------------------===//
//
// Part of the Jeandle JIT compiler.
//
// Real partial-escape semantics. When a virtual object encounters an
// instruction the analyzer can't fold (an "escape" point — generic call,
// ret, store of the virtual into non-virtual memory, etc.) we record a
// Materialize effect at that point. The transform pass re-emits a real
// allocation immediately before the escape, replays the tracked field
// stores, and RAUWs the original allocation's result with the new one. The
// original allocation invoke is still erased via EliminateAllocation; the
// virtually-folded stores/loads before the escape still apply.
//
// Multi-predecessor merge: every block's per-object exit state (Virtual /
// Materialized, tracked field values, lock counts) is snapshotted into
// BlockExits; at the top of every block we reconstruct the per-block state
// either by inheriting from the unique pred's snapshot or by running
// mergeStates() over all predecessors' snapshots. An object stays virtual
// at BB's entry IFF every predecessor reports it Virtual, all predecessors
// agree on the tracked field values at every offset, AND all predecessors
// agree on the lock count. Any disagreement (mixed virtual / materialized,
// missing on some path, field mismatch, lock mismatch) marks the object
// ineligible — the commit() sweep drops every recorded effect for it and
// the original IR survives unchanged. Explicit LLVM PHIs of java-heap
// pointers are also walked: Case B (every incoming resolves to the same
// ObjectID and the object is still virtual at merge entry) registers the
// PHI as a virtual alias; Case A (mixed) marks every virtual incoming
// ineligible.
//
// Current simplifications (carried as TODOs):
//   * No PHI Case C: when incomings carry different ObjectIDs we treat the
//     merge as Case A (ineligible) rather than synthesizing a merged virtual.
//   * No iterative stabilization: the merge is a single pass; we don't
//     re-process other successors of a predecessor whose state changed.
//
// Lock cascade: when materializing a virtual whose LockCount > 0, the
// analyzer (1) drops the previously-recorded ReplaceCall(true) effects
// targeting the unbalanced enter call sites, (2) emits ReplaceInput effects
// retargeting each enter's first operand onto the materialized pointer, and
// (3) clears the live stack. Matching exits downstream of the escape point
// are not elided in the first place (foldMonitorExit on a materialized
// object returns false) and survive in IR. Under
// JeandleAssumeStrictLockOrder (default true), every other still-locked
// virtual is also cascaded into materialization at the same insertion point.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Jeandle/PartialEscapeAnalysis.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Jeandle/JavaType.h"
#include "llvm/IR/Jeandle/Metadata.h"
#include "llvm/IR/Jeandle/PartialEscape.h"
#include "llvm/IR/Jeandle/VMCallback.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Jeandle/PartialEscapeUtils.h"

using namespace llvm;

// Cascade other still-locked virtuals at materialization when the target
// uses HotSpot's lightweight locking (LM_LIGHTWEIGHT requires strict lock
// nesting on the runtime stack). No VMCallback exposes this today, so default
// to true: JDK 21+ uses LM_LIGHTWEIGHT on x86_64 by default.
// TODO: wire to a real RequiresStrictLockOrder VM callback.
static llvm::cl::opt<bool> JeandleAssumeStrictLockOrder(
    "jeandle-assume-strict-lock-order", llvm::cl::init(true), llvm::cl::Hidden,
    llvm::cl::desc("PEA: assume target VM requires strict lock nesting; "
                   "cascade still-locked virtuals on materialization."));

AnalysisKey PartialEscapeAnalysis::Key;

namespace {

// Cap on array length for virtualization candidates. Mirrors the PEA-Plan
// section 2.3.2 guidance ("MaximumEscapeAnalysisArrayLength"). Kept as a
// translation-unit-local constant for now; a JVM-tunable flag can be wired
// up later without churn to the data structures.
constexpr uint32_t MaximumEscapeAnalysisArrayLength = 32;

// Per-block-exit snapshot of the analyzer's per-object state. We record
// this AFTER processing a block so successors can either inherit directly
// (single live pred) or merge across multiple preds. Keeping the state
// per-block — rather than one global accumulating state — is what makes
// multi-pred merge correct in the presence of branches that mutate the field
// values of a virtual independently.
struct BlockExitInfo {
  DenseSet<jeandle::ObjectID> Virtuals;
  DenseSet<jeandle::ObjectID> Materialized;
  DenseMap<jeandle::ObjectID, DenseMap<int64_t, jeandle::FieldValue>>
      FieldStates;
  DenseMap<jeandle::ObjectID, unsigned> LockCounts;
  // Per-object live monitorenter stack at block exit. Each entry is an
  // unbalanced monitorenter call site (i.e. its matching monitorexit hasn't
  // been seen yet on this path). Sized identically to LockCounts[ID]. Used
  // by materializeAt to undo only the path-relevant elisions.
  DenseMap<jeandle::ObjectID, SmallVector<llvm::CallBase *, 4>> LiveLockEnters;
  // Per-object materialized Value* at block exit. For an object in
  // Materialized, this is the LLVM pointer that downstream merges should use
  // as the PHI input on this predecessor edge. Initially the placeholder is
  // the original allocation (VObj.AllocationCall); the transform redirects
  // through MatPerBlock at apply time.
  DenseMap<jeandle::ObjectID, Value *> MaterializedValues;
};

class Analyzer {
public:
  Analyzer(Function &F, DominatorTree &DT, LoopInfo &LI)
      : F(F), DT(DT), LI(LI), DL(F.getParent()->getDataLayout()) {}

  jeandle::PEAResult run();

private:
  Function &F;
  DominatorTree &DT;
  LoopInfo &LI;
  const DataLayout &DL;
  jeandle::PEAResult Result;
  jeandle::AliasMap Aliases;
  // Per-block accumulating object state. Reset at the top of every block from
  // the predecessor snapshots (single inherit / multi-pred merge); rebuilt as
  // the instructions in the block are processed; snapshotted to BlockExits at
  // the end of the block.
  jeandle::PEABlockState CurrentState;

  // Per-object field state: ObjectID -> (offset -> FieldValue). Decoupled from
  // ObjectState::Entries because field discovery is lazy and we don't want to
  // keep VirtualObject::Fields and ObjectState::Entries in lock step.
  DenseMap<jeandle::ObjectID, DenseMap<int64_t, jeandle::FieldValue>>
      FieldStates;

  // Per-object eligibility flag. Function-wide: starts true at allocation;
  // flipped to false on any escape (non-constant offset, type mismatch,
  // nested-virtual store/load, opaque consumer, incompatible multi-pred
  // merge). Once false, commit() drops every recorded effect for the object.
  DenseMap<jeandle::ObjectID, bool> Eligible;

  // Per-object monitor lock counter. Incremented on a folded monitorenter,
  // decremented on a folded monitorexit. Any object with LockCount != 0 at
  // commit time is marked ineligible (unbalanced locking).
  DenseMap<jeandle::ObjectID, unsigned> LockCounts;

  // Per-block live monitorenter stack per ObjectID. Pushed by a folded
  // monitorenter on the receiver, popped by the matching monitorexit. At any
  // point the stack contains exactly the unbalanced enter call sites whose
  // matching exits haven't been seen yet on this path; size == LockCounts[ID].
  // Reset+inherited per block (same lifecycle as LockCounts) so siblings in
  // a diamond CFG don't share stacks. materializeAt walks this stack to
  // decide which ReplaceCall elisions to undo. Mirrors Graal's `obj.locks`
  // linked list, which is per-state and inherited via PartialEscapeBlockState.
  DenseMap<jeandle::ObjectID, SmallVector<CallBase *, 4>> LiveLockEnters;

  // Per-path "this object has already been materialized somewhere upstream"
  // set. Materialization is recorded at most once per ObjectID per pred path
  // — the first escape site wins; multi-pred merges that see the object
  // materialized on every incoming carry the Materialized state forward.
  DenseSet<jeandle::ObjectID> Materialized;

  // Per-block exit snapshots, keyed by the block that produced them.
  DenseMap<BasicBlock *, BlockExitInfo> BlockExits;

  // Function-wide dedup of (Pred, ObjectID) materializations. Multiple
  // merge-time Materialize-at-pred emissions for the same (Pred, ObjectID)
  // would otherwise produce duplicate invokes; this set ensures we emit
  // exactly one Materialize effect per (Pred, ObjectID) pair across the
  // entire run.
  DenseMap<BasicBlock *, DenseSet<jeandle::ObjectID>> MaterializedAtPred;

  void processBlock(BasicBlock *BB);
  void applyThreeTier(Instruction *I);

  // Per-block state helpers.
  void resetPerBlockState();
  void inheritFromExit(const BlockExitInfo &Exit);
  void mergeStates(BasicBlock *BB);
  void snapshotExitState(BasicBlock *BB);
  void processBlockPhis(BasicBlock *BB);

  PHINode *createUnparentedPhi(Type *Ty, unsigned N, const Twine &Name);

  // Produce a Value* of type LoadTy semantically equal to V, possibly
  // synthesizing an unparented coercion instruction (registered with
  // Result.OwnedInsts) that the transform's ReplaceLoad handler will splice
  // in immediately before the load. Returns V unchanged if no coercion is
  // needed, or nullptr if no safe coercion exists; callers should bail to
  // ineligible in the latter case. InsertContext is the load whose DebugLoc
  // (if any) is propagated onto the synthesized cast.
  Value *coerceToType(Value *V, Type *LoadTy, Instruction *InsertContext);

  // Loop-soundness helpers.
  void materializeBeforeLoops();
  void materializeAtPredFromExitInfo(jeandle::ObjectID ID, BasicBlock *PH,
                                     BlockExitInfo &ExitInfo);

  void tier1Allocate(CallBase *CB);
  void tier2Store(StoreInst *SI);
  void tier2Load(LoadInst *LI);
  bool tier2JavaOpFold(CallBase *CB);
  bool foldArrayLength(CallBase *CB);
  bool foldLoadKlass(CallBase *CB);
  bool foldCheckCast(CallBase *CB);
  bool foldCheckInstanceOf(CallBase *CB);
  bool foldInstanceOf(CallBase *CB);
  bool foldMonitorEnter(CallBase *CB);
  bool foldMonitorExit(CallBase *CB);
  bool foldArrayStoreCheck(CallBase *CB);
  bool foldCheckIfValueBased(CallBase *CB);
  // Common helper for checkcast/check_instanceof: returns the folded constant
  // bool (true/false) if the relationship is statically known, or
  // std::nullopt otherwise.
  std::optional<bool> evalSubtypeRelation(uintptr_t SubKlass,
                                          uintptr_t SuperKlass);
  void emitReplaceCall(CallBase *CB, Value *Replacement, jeandle::ObjectID ID);
  void materializeAllVirtualOperands(Instruction *I);
  void materializeAt(jeandle::ObjectID ID, Instruction *InsertBefore);
  void propagatePointerAlias(Instruction *I);

  void commit();
  void dropEffectsFor(jeandle::ObjectID ID);
};

void Analyzer::processBlock(BasicBlock *BB) {
  // Rebuild per-block state from the predecessor snapshots before we walk
  // instructions. Entry block starts empty. Single-pred blocks inherit.
  // Multi-pred blocks run the merge (mergeStates also handles the degenerate
  // "no processed preds" case, which can happen e.g. on irreducible loop
  // headers where the back-edge pred hasn't been visited yet — we treat
  // those conservatively as starting empty).
  resetPerBlockState();
  if (BB == &F.getEntryBlock()) {
    // Entry: nothing to inherit; per-block state is empty.
  } else if (BB->hasNPredecessors(1)) {
    BasicBlock *P = *predecessors(BB).begin();
    auto It = BlockExits.find(P);
    if (It != BlockExits.end())
      inheritFromExit(It->second);
  } else {
    mergeStates(BB);
  }

  // Walk the explicit LLVM PHIs at the top of BB. PHIs that resolve to the
  // same ObjectID on every incoming carry the virtual alias forward; mixed
  // PHIs mark every virtual incoming ineligible (Case A simplification).
  processBlockPhis(BB);

  for (Instruction &I : *BB) {
    applyThreeTier(&I);
  }

  snapshotExitState(BB);
}

void Analyzer::resetPerBlockState() {
  CurrentState = jeandle::PEABlockState();
  FieldStates.clear();
  LockCounts.clear();
  LiveLockEnters.clear();
  Materialized.clear();
}

void Analyzer::inheritFromExit(const BlockExitInfo &Exit) {
  for (jeandle::ObjectID ID : Exit.Virtuals) {
    if (!Eligible.lookup(ID))
      continue;
    CurrentState.addObject(ID, jeandle::ObjectState(/*numEntries=*/0));
  }
  for (jeandle::ObjectID ID : Exit.Materialized) {
    if (!Eligible.lookup(ID))
      continue;
    jeandle::VirtualObject &VObj = *Result.VirtualObjects[ID];
    jeandle::ObjectState OS(0);
    // Prefer the snapshot's MaterializedValues entry (e.g. a merge-block
    // PHI synthesized by an earlier mergeStates) over the OrigAlloc fallback.
    Value *MV = nullptr;
    auto MIt = Exit.MaterializedValues.find(ID);
    if (MIt != Exit.MaterializedValues.end())
      MV = MIt->second;
    if (!MV)
      MV = VObj.AllocationCall;
    OS.materialize(MV);
    CurrentState.addObject(ID, std::move(OS));
    Materialized.insert(ID);
  }
  for (auto &Kv : Exit.FieldStates) {
    if (!Eligible.lookup(Kv.first))
      continue;
    FieldStates[Kv.first] = Kv.second;
  }
  for (auto &Kv : Exit.LockCounts) {
    if (!Eligible.lookup(Kv.first))
      continue;
    if (Kv.second != 0)
      LockCounts[Kv.first] = Kv.second;
  }
  // Inherit the live monitorenter stack alongside LockCounts. The
  // CallBase* identities are still valid (we never erase IR during analysis).
  for (auto &Kv : Exit.LiveLockEnters) {
    if (!Eligible.lookup(Kv.first))
      continue;
    if (!Kv.second.empty())
      LiveLockEnters[Kv.first] = Kv.second;
  }
}

PHINode *Analyzer::createUnparentedPhi(Type *Ty, unsigned N,
                                       const Twine &Name) {
  PHINode *Phi = PHINode::Create(Ty, N, Name);
  Result.OwnedPhis.emplace_back(Phi);
  return Phi;
}

// Minimal type-coercion for tier2Load, plan §2.3.4 case A simplest sub-case.
// When a load's type doesn't match the stored Scalar's type, but the bit
// widths are equal and both sides are primitive scalars, emit a `bitcast`.
// Pointer↔primitive at the same slot is forbidden by invariant F2
// (PartialEscape.h): the slot's GC-ness must be stable, so a ref cannot be
// re-read as a primitive (or vice-versa) without materializing.
// Cross-addrspace pointer coercion is also bailed (rare, GC-risky).
// Different bit-widths are future work (would need trunc/lshr+trunc with
// offset-within-slot tracking).
Value *Analyzer::coerceToType(Value *V, Type *LoadTy,
                              Instruction *InsertContext) {
  Type *VTy = V->getType();
  if (VTy == LoadTy)
    return V;
  if (!VTy->isSized() || !LoadTy->isSized())
    return nullptr;
  uint64_t VBits = DL.getTypeSizeInBits(VTy);
  uint64_t LBits = DL.getTypeSizeInBits(LoadTy);
  if (VBits != LBits)
    return nullptr; // future work: trunc / lshr+trunc.
  // Invariant F2: ref↔primitive at the same slot must materialize.
  if (VTy->isPointerTy() != LoadTy->isPointerTy())
    return nullptr;
  // Pointer↔pointer: only safe if the addrspaces match (else GC-strategy
  // mismatch). Same-AS same-bitwidth pointers are already type-identical
  // because LLVM 17 uses opaque pointers, so we'd have returned above.
  // Defensive: bail anyway if we somehow see distinct AS.
  if (VTy->isPointerTy() && LoadTy->isPointerTy()) {
    if (VTy->getPointerAddressSpace() != LoadTy->getPointerAddressSpace())
      return nullptr;
    return V;
  }
  // Both primitives, same bit width → BitCast (Float↔Int, Half↔i16, etc.).
  // Use BitCastable as a sanity check; opaque-pointers-aware LLVM expects
  // pointer-vs-non-pointer to be filtered out above.
  if (!CastInst::isBitCastable(VTy, LoadTy))
    return nullptr;
  Instruction *Cast = CastInst::Create(Instruction::BitCast, V, LoadTy,
                                       "pea.coerce",
                                       /*InsertBefore=*/nullptr);
  if (InsertContext)
    Cast->setDebugLoc(InsertContext->getDebugLoc());
  Result.OwnedInsts.emplace_back(Cast);
  return Cast;
}

void Analyzer::mergeStates(BasicBlock *BB) {
  // Collect the snapshots of every predecessor we've already processed. RPO
  // guarantees forward-edge preds are visited first; back-edge preds are not
  // yet available and are silently skipped (the loop-preheader force-
  // materialization sweep handles loop soundness).
  SmallVector<BasicBlock *, 4> PredBBs;
  SmallVector<BlockExitInfo *, 4> Preds;
  for (BasicBlock *P : predecessors(BB)) {
    auto It = BlockExits.find(P);
    if (It == BlockExits.end())
      continue;
    PredBBs.push_back(P);
    Preds.push_back(&It->second);
  }
  if (Preds.empty())
    return; // nothing to inherit; start empty.
  if (Preds.size() == 1) {
    inheritFromExit(*Preds[0]);
    return;
  }

  // Compute the union of ObjectIDs that any predecessor knows about.
  DenseSet<jeandle::ObjectID> Union;
  for (const auto *P : Preds) {
    for (jeandle::ObjectID ID : P->Virtuals)
      Union.insert(ID);
    for (jeandle::ObjectID ID : P->Materialized)
      Union.insert(ID);
  }
  SmallVector<jeandle::ObjectID, 8> IDs(Union.begin(), Union.end());
  llvm::sort(IDs); // deterministic order for ineligibility/marking effects.

  for (jeandle::ObjectID ID : IDs) {
    if (!Eligible.lookup(ID))
      continue;

    // Per-pred disposition.
    bool AllVirtual = true;
    bool AllMaterialized = true;
    for (const auto *P : Preds) {
      bool V = P->Virtuals.count(ID);
      bool M = P->Materialized.count(ID);
      if (!V)
        AllVirtual = false;
      if (!M)
        AllMaterialized = false;
    }

    if (AllMaterialized) {
      // Every incoming path already materialized the object — carry the
      // materialized state forward at BB's entry. If the materialized pointer
      // differs across preds, synthesize a ptr addrspace(1) PHI; otherwise
      // reuse the unique pointer.
      jeandle::VirtualObject &VObj = *Result.VirtualObjects[ID];
      Value *Unique = nullptr;
      bool AllSame = true;
      for (auto *P : Preds) {
        auto It = P->MaterializedValues.find(ID);
        Value *V = (It != P->MaterializedValues.end()) ? It->second
                                                       : VObj.AllocationCall;
        if (!Unique) {
          Unique = V;
        } else if (Unique != V) {
          AllSame = false;
          break;
        }
      }
      Value *MV = Unique;
      if (!AllSame) {
        Type *PtrTy =
            PointerType::get(F.getContext(),
                              jeandle::AddrSpace::JavaHeapAddrSpace);
        PHINode *Phi = createUnparentedPhi(PtrTy, Preds.size(),
                                           "pea.materialized.phi");
        jeandle::PEAResult::Effect PE;
        PE.Kind = jeandle::PEAResult::EffectKind::CreatePHI;
        PE.Block = BB;
        PE.SeqNo = Result.nextSeqNo();
        PE.ObjID = ID;
        PE.PhiInst = Phi;
        PE.PHIType = PtrTy;
        for (unsigned i = 0; i < Preds.size(); ++i) {
          auto It = Preds[i]->MaterializedValues.find(ID);
          Value *V = (It != Preds[i]->MaterializedValues.end())
                         ? It->second
                         : VObj.AllocationCall;
          PE.PHIIncomingValues.push_back(V);
          PE.PHIIncomingBlocks.push_back(PredBBs[i]);
        }
        Result.addBlockEffect(std::move(PE));
        MV = Phi;
      }
      jeandle::ObjectState OS(0);
      OS.materialize(MV);
      CurrentState.addObject(ID, std::move(OS));
      Materialized.insert(ID);
      continue;
    }

    if (!AllVirtual) {
      // Mixed (virtual on some paths, materialized or missing on others).
      // Short-circuit when the object isn't tracked on every pred (e.g.,
      // an LLVM PHI mixing a virtual incoming from one branch and an
      // unrelated pointer from another). The merge entry simply doesn't
      // carry this ObjectID; processBlockPhis below picks up any LLVM PHI
      // that references it and materializes the virtual incomings.
      bool MissingOnSomePred = false;
      for (auto *P : Preds) {
        if (!P->Materialized.count(ID) && !P->Virtuals.count(ID)) {
          MissingOnSomePred = true;
          break;
        }
      }
      if (MissingOnSomePred) {
        continue;
      }

      // True mixed virtual+materialized. Inherit Materialized at the merge
      // with the OrigAlloc placeholder. The safe-IP-hoisted materializeAt
      // design (driven by an upstream escape on the materialized pred) emits
      // a single materialization that dominates the merge in the common
      // diamond-CFG case; the transform's RAUW snaps every existing IR use
      // of OrigAlloc onto the live NewInv, so downstream uses at the merge
      // resolve correctly without an explicit ptr addrspace(1) PHI here.
      // TODO: when alloc doesn't dominate merge, emit a true per-pred
      // materialization + ptr addrspace(1) PHI and redirect downstream uses
      // via a DT-aware fix-up pass.
      jeandle::VirtualObject &VObj = *Result.VirtualObjects[ID];
      jeandle::ObjectState OS(0);
      OS.materialize(VObj.AllocationCall);
      CurrentState.addObject(ID, std::move(OS));
      Materialized.insert(ID);
      continue;
    }

    // All preds report Virtual: check lock counts.
    unsigned RefLC = Preds[0]->LockCounts.lookup(ID);
    bool LocksMatch = true;
    for (const auto *P : Preds) {
      if (P->LockCounts.lookup(ID) != RefLC) {
        LocksMatch = false;
        break;
      }
    }
    if (!LocksMatch) {
      // Any lock-count mismatch forces escape.
      // TODO: cascade the missing lock/unlock pairs on the materialization
      // paths.
      Eligible[ID] = false;
      continue;
    }
    // When LockCounts agree we also require the live enter-stacks to be
    // identical (same calls in same order) so that any later materializeAt
    // undoes a single, well-defined stack. If different paths entered
    // different monitor calls, the un-elision becomes path-dependent and
    // we don't model that — bail.
    if (RefLC != 0) {
      const auto &RefStack = Preds[0]->LiveLockEnters.lookup(ID);
      bool StacksMatch = true;
      for (const auto *P : Preds) {
        const auto &S = P->LiveLockEnters.lookup(ID);
        if (S.size() != RefStack.size()) {
          StacksMatch = false;
          break;
        }
        for (unsigned i = 0; i < S.size(); ++i) {
          if (S[i] != RefStack[i]) {
            StacksMatch = false;
            break;
          }
        }
        if (!StacksMatch)
          break;
      }
      if (!StacksMatch) {
        Eligible[ID] = false;
        continue;
      }
    }

    // Check field states at every tracked offset. Identical entries flow
    // straight into Merged; disagreements trigger field-PHI synthesis.
    DenseSet<int64_t> Offsets;
    for (const auto *P : Preds) {
      auto FIt = P->FieldStates.find(ID);
      if (FIt == P->FieldStates.end())
        continue;
      for (auto &Kv : FIt->second)
        Offsets.insert(Kv.first);
    }
    SmallVector<int64_t, 8> SortedOffsets(Offsets.begin(), Offsets.end());
    llvm::sort(SortedOffsets); // determinism
    bool BailObject = false;
    DenseMap<int64_t, jeandle::FieldValue> Merged;
    // Snapshot of newly-emitted CreatePHI effects for this object's fields;
    // committed to Result only if every offset succeeds.
    SmallVector<jeandle::PEAResult::Effect, 4> PendingPhiEffects;
    for (int64_t Off : SortedOffsets) {
      jeandle::FieldValue Ref = jeandle::FieldValue::unknown();
      bool HaveRef = false;
      bool Disagrees = false;
      for (const auto *P : Preds) {
        jeandle::FieldValue FV = jeandle::FieldValue::unknown();
        auto FIt = P->FieldStates.find(ID);
        if (FIt != P->FieldStates.end()) {
          auto OIt = FIt->second.find(Off);
          if (OIt != FIt->second.end())
            FV = OIt->second;
        }
        if (!HaveRef) {
          Ref = FV;
          HaveRef = true;
        } else if (!Ref.shallowEquals(FV)) {
          Disagrees = true;
          break;
        }
      }
      if (!Disagrees) {
        if (!Ref.isUnknown())
          Merged[Off] = Ref;
        continue;
      }

      // Field disagreement at Off — attempt field-PHI synthesis. Decide
      // the merged PHI type. Compatibility rule: every non-unknown entry's
      // declared type must be identical, OR every non-unknown entry must be
      // a pointer in the Java heap addrspace (in which case the PHI is
      // ptr addrspace(1) and ref/scalar are interchangeable).
      Type *PhiType = nullptr;
      bool AllPointer = true;
      for (const auto *P : Preds) {
        auto FIt = P->FieldStates.find(ID);
        if (FIt == P->FieldStates.end())
          continue;
        auto OIt = FIt->second.find(Off);
        if (OIt == FIt->second.end())
          continue;
        const jeandle::FieldValue &FV = OIt->second;
        if (FV.isUnknown())
          continue;
        Type *T = nullptr;
        if (FV.isScalar())
          T = FV.getScalar()->getType();
        else
          T = FV.getDeclaredType();
        if (!T) {
          BailObject = true;
          break;
        }
        if (!T->isPointerTy() ||
            T->getPointerAddressSpace() !=
                jeandle::AddrSpace::JavaHeapAddrSpace)
          AllPointer = false;
        if (!PhiType)
          PhiType = T;
        else if (PhiType != T && !(PhiType->isPointerTy() && T->isPointerTy() &&
                                    PhiType->getPointerAddressSpace() ==
                                        jeandle::AddrSpace::JavaHeapAddrSpace &&
                                    T->getPointerAddressSpace() ==
                                        jeandle::AddrSpace::JavaHeapAddrSpace))
          BailObject = true;
        if (BailObject)
          break;
      }
      if (BailObject)
        break;
      if (!PhiType) {
        // Should be unreachable — a disagreement implies at least two
        // distinct non-unknown entries.
        BailObject = true;
        break;
      }
      if (AllPointer)
        PhiType = PointerType::get(F.getContext(),
                                    jeandle::AddrSpace::JavaHeapAddrSpace);

      // Compute per-pred input value.
      SmallVector<Value *, 4> InValues;
      InValues.reserve(Preds.size());
      bool LocalBail = false;
      for (unsigned i = 0; i < Preds.size(); ++i) {
        jeandle::FieldValue FV = jeandle::FieldValue::unknown();
        auto FIt = Preds[i]->FieldStates.find(ID);
        if (FIt != Preds[i]->FieldStates.end()) {
          auto OIt = FIt->second.find(Off);
          if (OIt != FIt->second.end())
            FV = OIt->second;
        }
        Value *In = nullptr;
        if (FV.isUnknown()) {
          In = jeandle::FieldValue::defaultFor(PhiType);
        } else if (FV.isScalar()) {
          Value *V = FV.getScalar();
          if (V->getType() != PhiType) {
            // Scalar with a non-matching primitive type at a pointer slot,
            // or vice versa.
            LocalBail = true;
            break;
          }
          In = V;
        } else if (FV.isMaterializedRef()) {
          if (!PhiType->isPointerTy()) {
            LocalBail = true;
            break;
          }
          In = FV.getMaterialized();
        } else if (FV.isVirtualRef()) {
          if (!PhiType->isPointerTy()) {
            LocalBail = true;
            break;
          }
          jeandle::ObjectID InnerID = FV.getVirtualRef();
          // Materialize the inner object at this pred's terminator. After
          // this, the field's effective input value is OrigAlloc(inner) —
          // the transform's MatPerBlock substitutes it with NewInv at apply.
          materializeAtPredFromExitInfo(InnerID, PredBBs[i], *Preds[i]);
          if (!Eligible.lookup(InnerID)) {
            LocalBail = true;
            break;
          }
          jeandle::VirtualObject &InnerVO =
              *Result.VirtualObjects[InnerID];
          In = InnerVO.AllocationCall;
        } else {
          LocalBail = true;
          break;
        }
        InValues.push_back(In);
      }
      if (LocalBail) {
        BailObject = true;
        break;
      }

      PHINode *Phi = createUnparentedPhi(PhiType, Preds.size(),
                                          "pea.field.phi");
      jeandle::PEAResult::Effect PE;
      PE.Kind = jeandle::PEAResult::EffectKind::CreatePHI;
      PE.Block = BB;
      PE.SeqNo = Result.nextSeqNo();
      PE.ObjID = ID;
      PE.PhiInst = Phi;
      PE.PHIType = PhiType;
      for (unsigned i = 0; i < Preds.size(); ++i) {
        PE.PHIIncomingValues.push_back(InValues[i]);
        PE.PHIIncomingBlocks.push_back(PredBBs[i]);
      }
      PendingPhiEffects.push_back(std::move(PE));

      if (PhiType->isPointerTy())
        Merged[Off] = jeandle::FieldValue::materializedRef(Phi);
      else
        Merged[Off] = jeandle::FieldValue::scalar(Phi);
    }
    if (BailObject) {
      Eligible[ID] = false;
      continue;
    }

    // Commit any synthesized field PHIs to Result.
    for (auto &PE : PendingPhiEffects)
      Result.addBlockEffect(std::move(PE));

    // Case B: object stays virtual at BB entry with the merged field state.
    CurrentState.addObject(ID, jeandle::ObjectState(/*numEntries=*/0));
    if (!Merged.empty())
      FieldStates[ID] = std::move(Merged);
    if (RefLC != 0) {
      LockCounts[ID] = RefLC;
      // The merged live stack is identical to (any) pred's stack, since the
      // StacksMatch check above succeeded.
      const auto &RefStack = Preds[0]->LiveLockEnters.lookup(ID);
      if (!RefStack.empty())
        LiveLockEnters[ID] = RefStack;
    }
  }
}

void Analyzer::snapshotExitState(BasicBlock *BB) {
  BlockExitInfo Info;
  for (auto &VObjUP : Result.VirtualObjects) {
    jeandle::ObjectID ID = VObjUP->getID();
    if (!Eligible.lookup(ID))
      continue;
    const jeandle::ObjectState *OS = CurrentState.getObjectStateOptional(ID);
    if (!OS)
      continue;
    if (OS->isVirtual()) {
      Info.Virtuals.insert(ID);
      auto FIt = FieldStates.find(ID);
      if (FIt != FieldStates.end() && !FIt->second.empty())
        Info.FieldStates[ID] = FIt->second;
      auto LIt = LockCounts.find(ID);
      if (LIt != LockCounts.end() && LIt->second != 0)
        Info.LockCounts[ID] = LIt->second;
      // Snapshot the live monitorenter stack so successor blocks see the
      // same path-specific call set we did.
      auto SIt = LiveLockEnters.find(ID);
      if (SIt != LiveLockEnters.end() && !SIt->second.empty())
        Info.LiveLockEnters[ID] = SIt->second;
    } else if (OS->isMaterialized()) {
      Info.Materialized.insert(ID);
      // Capture the materialized pointer at block exit so downstream
      // merges can use it as the PHI input on this edge.
      Info.MaterializedValues[ID] = OS->getMaterializedValue();
    }
  }
  BlockExits[BB] = std::move(Info);
}

void Analyzer::processBlockPhis(BasicBlock *BB) {
  // Walk explicit LLVM PHIs of java-heap pointers. Other PHIs (e.g., scalar
  // i32 PHIs of folded virtual-load results) flow through normal SSA and are
  // not the concern of the analyzer.
  for (PHINode &Phi : BB->phis()) {
    Type *Ty = Phi.getType();
    if (!Ty->isPointerTy())
      continue;
    if (cast<PointerType>(Ty)->getAddressSpace() !=
        jeandle::AddrSpace::JavaHeapAddrSpace)
      continue;

    // Resolve each incoming against its predecessor's exit snapshot. We only
    // check the AliasMap (functions like full resolveVirtualRef would need a
    // pred-specific PEABlockState, which we don't have on hand); pointer
    // PHIs whose incomings are themselves derived through GEP/cast chains
    // are uncommon enough that the alias-only resolution is sufficient.
    // Note that the AliasMap is function-wide so a virtual alias registered
    // in any block is visible here.
    SmallVector<std::optional<jeandle::ObjectID>, 4> InIDs;
    bool AnyVirtual = false;
    InIDs.reserve(Phi.getNumIncomingValues());
    for (unsigned I = 0; I < Phi.getNumIncomingValues(); ++I) {
      BasicBlock *Pred = Phi.getIncomingBlock(I);
      Value *V = Phi.getIncomingValue(I);
      std::optional<jeandle::ObjectID> Found;
      auto AID = Aliases.getVirtualAlias(V);
      auto PredIt = BlockExits.find(Pred);
      if (AID && PredIt != BlockExits.end() &&
          PredIt->second.Virtuals.count(*AID))
        Found = *AID;
      InIDs.push_back(Found);
      if (Found)
        AnyVirtual = true;
    }
    if (!AnyVirtual)
      continue;

    // Case B: every incoming agrees on the same ObjectID AND the object is
    // still virtual at merge entry (mergeStates kept it).
    bool AllSame = true;
    std::optional<jeandle::ObjectID> First;
    for (auto &O : InIDs) {
      if (!O) {
        AllSame = false;
        break;
      }
      if (!First)
        First = O;
      else if (*First != *O) {
        AllSame = false;
        break;
      }
    }

    if (AllSame && First) {
      const jeandle::ObjectState *OS =
          CurrentState.getObjectStateOptional(*First);
      if (OS && OS->isVirtual()) {
        // Register the PHI as an alias for the same ObjectID so downstream
        // load/store handlers in this block resolve through it.
        Aliases.addVirtualAlias(&Phi, *First);
        continue;
      }
    }

    // Case A: mixed virtual + non-virtual incomings, OR different ObjectIDs
    // across incomings. For every virtual incoming, materialize at that
    // incoming's predecessor terminator. The PHI itself stays in IR;
    // transform-time RAUW (and MatPerBlock substitution at PHI nodes we
    // synthesize elsewhere) handles operand updates. We do NOT synthesize
    // a merged ObjectID for Case C — that's left for a later task.
    for (unsigned I = 0; I < Phi.getNumIncomingValues(); ++I) {
      if (!InIDs[I])
        continue;
      BasicBlock *Pred = Phi.getIncomingBlock(I);
      auto PredIt = BlockExits.find(Pred);
      if (PredIt == BlockExits.end()) {
        Eligible[*InIDs[I]] = false;
        continue;
      }
      materializeAtPredFromExitInfo(*InIDs[I], Pred, PredIt->second);
    }
  }
}

void Analyzer::applyThreeTier(Instruction *I) {
  // Tier 1: Jeandle allocation site.
  if (auto *CB = dyn_cast<CallBase>(I)) {
    if (jeandle::pea::isJeandleAllocation(CB)) {
      tier1Allocate(CB);
      return;
    }
  }

  // Tier 2 store/load — try them regardless of the hasVirtualInputs gate.
  // tier2Store/Load resolve the pointer through GEPs/casts and early-exit if
  // it doesn't bottom out on a virtual base.
  if (auto *SI = dyn_cast<StoreInst>(I)) {
    tier2Store(SI);
    return;
  }
  if (auto *LI = dyn_cast<LoadInst>(I)) {
    tier2Load(LI);
    return;
  }

  // Other tier-2 / tier-3 consumers.
  if (Aliases.hasVirtualInputs(I)) {
    // Pointer-derivation forwards the virtual alias to the derived pointer so
    // downstream load/store handlers can pick up the base via the alias map.
    if (isa<GetElementPtrInst>(I) || isa<BitCastInst>(I) ||
        isa<AddrSpaceCastInst>(I) || isa<FreezeInst>(I)) {
      propagatePointerAlias(I);
      return;
    }
    // §2.3.14: known non-escaping LLVM intrinsics (assume, lifetime markers,
    // invariant markers, debug intrinsics, ...) are no-ops for PEA. The
    // virtual stays virtual and the call is left alone in IR (some are
    // DCE'd downstream; others are harmless). Must run BEFORE the JavaOp
    // fold + generic-escape fall-through.
    if (auto *II = dyn_cast<IntrinsicInst>(I)) {
      switch (II->getIntrinsicID()) {
      case Intrinsic::assume:
      case Intrinsic::lifetime_start:
      case Intrinsic::lifetime_end:
      case Intrinsic::invariant_start:
      case Intrinsic::invariant_end:
      case Intrinsic::experimental_noalias_scope_decl:
      case Intrinsic::dbg_declare:
      case Intrinsic::dbg_value:
      case Intrinsic::dbg_label:
      case Intrinsic::donothing:
      case Intrinsic::sideeffect:
        return;
      default:
        break;
      }
    }
    // §2.3.11/§2.3.12: equality compare against a virtual pointer folds.
    // Virtual objects are never null (by construction they track an in-flight
    // alloc), so `icmp eq virt, null` -> false, `icmp ne virt, null` -> true.
    // Two virtuals: same ID -> eq=true; different IDs -> eq=false.
    // Mixed virtual + non-null non-virtual pointer: identity differs -> eq
    // folds to false.
    if (auto *ICmp = dyn_cast<ICmpInst>(I)) {
      if (ICmp->isEquality()) {
        Value *Op0 = ICmp->getOperand(0);
        Value *Op1 = ICmp->getOperand(1);
        auto V0 = jeandle::pea::resolveVirtualRef(Op0, CurrentState, Aliases,
                                                  DL);
        auto V1 = jeandle::pea::resolveVirtualRef(Op1, CurrentState, Aliases,
                                                  DL);
        bool Op0IsNull = isa<ConstantPointerNull>(Op0);
        bool Op1IsNull = isa<ConstantPointerNull>(Op1);
        bool Folded = false;
        bool EqResult = false;
        jeandle::ObjectID BaseID = jeandle::InvalidObjectID;
        if (V0 && Op1IsNull) {
          Folded = true;
          EqResult = false;
          BaseID = *V0;
        } else if (V1 && Op0IsNull) {
          Folded = true;
          EqResult = false;
          BaseID = *V1;
        } else if (V0 && V1) {
          Folded = true;
          EqResult = (*V0 == *V1);
          BaseID = *V0;
        } else if (V0 && !V1 && !Op1IsNull) {
          // Virtual vs. non-virtual non-null pointer: distinct identity.
          Folded = true;
          EqResult = false;
          BaseID = *V0;
        } else if (V1 && !V0 && !Op0IsNull) {
          Folded = true;
          EqResult = false;
          BaseID = *V1;
        }
        if (Folded) {
          bool IsEq = (ICmp->getPredicate() == ICmpInst::ICMP_EQ);
          bool FinalResult = IsEq ? EqResult : !EqResult;
          Constant *C = ConstantInt::get(ICmp->getType(),
                                         FinalResult ? 1 : 0);
          // Reuse ReplaceLoad: its handler does generic Instruction RAUW +
          // erase, which is exactly what we need here.
          jeandle::PEAResult::Effect E;
          E.Kind = jeandle::PEAResult::EffectKind::ReplaceLoad;
          E.Block = ICmp->getParent();
          E.Target = ICmp;
          E.Replacement = C;
          E.SeqNo = Result.nextSeqNo();
          E.ObjID = BaseID;
          Result.addBlockEffect(std::move(E));
          Aliases.addScalarAlias(ICmp, C);
          return;
        }
      }
      // Non-equality ICmp on virtual heap pointers (slt/sgt/...) is UB on
      // GC pointers; conservatively materialize.
    }
    // Recognise JavaOps that read/inspect a virtual receiver and try to
    // constant-fold them. tier2JavaOpFold returns true if the JavaOp was
    // handled (whether by folding to a constant or by being a known-safe
    // non-escaping shape that needs no transform).
    if (auto *CB = dyn_cast<CallBase>(I)) {
      if (tier2JavaOpFold(CB))
        return;
      // Fall through to the generic-escape path for unrecognised calls.
    }
    // Any other consumer of a virtual operand triggers materialization.
    materializeAllVirtualOperands(I);
    return;
  }

  // Tier 3 (scalar-replaced inputs): nothing to do.
}

void Analyzer::tier1Allocate(CallBase *CB) {
  // Loop-body allocations are permitted as virtualization candidates. The
  // dominance check in materializeAt (DT.dominates(VI, SafeIP) per
  // FieldStates entry) is what protects the analyzer from forming an unsound
  // replay: any field whose stored value is defined later in the loop body
  // cannot dominate the SafeIP just after the alloc, so the object becomes
  // ineligible and survives in IR untouched. materializeBeforeLoops
  // independently force-materializes any virtual that is still virtual at a
  // loop preheader's exit, so objects allocated BEFORE the loop and
  // surviving into the loop are also handled.
  // TODO: implement a full Graal-style loop fixpoint with snapshot/rollback
  // and MATERIALIZE_ALL escalation. That would recover alloc-before-loop
  // with body-side field mutation and true cross-iteration virtual tracking.
  // For now, the residual gaps are intentional.

  uintptr_t Klass = jeandle::pea::extractAllocationKlass(CB);
  if (Klass == 0)
    return;

  const bool IsInstance = jeandle::pea::isJeandleNewInstance(CB);
  const bool IsArray = jeandle::pea::isJeandleNewArray(CB);
  assert((IsInstance ^ IsArray) && "allocation must be either instance or array");

  std::unique_ptr<jeandle::VirtualObject> VO;

  if (IsInstance) {
    auto Size = jeandle::pea::extractInstanceSize(CB);
    if (!Size)
      return;
    VO = std::make_unique<jeandle::VirtualObject>(
        jeandle::InvalidObjectID, jeandle::VirtualObject::Instance, CB);
    VO->Klass = Klass;
    VO->SizeInBytes = *Size;
  } else {
    auto Length = jeandle::pea::extractArrayLength(CB);
    if (!Length)
      return;
    if (*Length > MaximumEscapeAnalysisArrayLength)
      return;
    VO = std::make_unique<jeandle::VirtualObject>(
        jeandle::InvalidObjectID, jeandle::VirtualObject::Array, CB);
    VO->Klass = Klass;
    VO->ArrayLength = *Length;
    if (CB->arg_size() >= 2)
      VO->ArrayLengthVal = CB->getArgOperand(1);
  }

  jeandle::ObjectID ID = Result.createVirtualObject(std::move(VO));
  Aliases.addVirtualAlias(CB, ID);
  // Register a Virtual ObjectState with zero entries. resolveVirtualRef only
  // needs the slot to be present + Kind == Virtual; the per-field FieldValue
  // tracking lives in FieldStates (see class comment).
  CurrentState.addObject(ID, jeandle::ObjectState(/*numEntries=*/0));
  Eligible[ID] = true;

  jeandle::PEAResult::Effect E;
  E.Kind = jeandle::PEAResult::EffectKind::EliminateAllocation;
  E.Block = CB->getParent();
  E.Target = CB;
  E.SeqNo = Result.nextSeqNo();
  E.ObjID = ID;
  Result.addBlockEffect(std::move(E));

  ++Result.VirtualizationDelta;
  --Result.AllocationDelta;
}

void Analyzer::tier2Store(StoreInst *SI) {
  Value *Ptr = SI->getPointerOperand();
  Value *Val = SI->getValueOperand();

  auto BaseID = jeandle::pea::resolveVirtualRef(Ptr, CurrentState, Aliases, DL);
  if (!BaseID)
    return;

  auto Offset = jeandle::pea::resolveFieldOffset(Ptr, DL);
  if (!Offset) {
    // Non-constant offset access — punt on materialization.
    Eligible[*BaseID] = false;
    return;
  }

  // Type-overlap validation via VirtualObject::getOrCreateFieldIndex. We don't
  // actually use the returned index (FieldStates is keyed by raw offset), but
  // -1 means an overlap/size conflict that forces escape.
  jeandle::VirtualObject &VObj = *Result.VirtualObjects[*BaseID];
  if (VObj.getOrCreateFieldIndex(*Offset, Val->getType()) < 0) {
    Eligible[*BaseID] = false;
    return;
  }

  // Compute the FieldValue for the stored Value.
  if (auto RefID =
          jeandle::pea::resolveVirtualRef(Val, CurrentState, Aliases, DL)) {
    // Nested virtual reference. Recursive materialization handles this at
    // materialize time by first materializing the inner object then storing
    // its materialized pointer into the outer's field. We record the nested
    // reference here and let materializeAt rewrite it later.
    FieldStates[*BaseID][*Offset] =
        jeandle::FieldValue::virtualRef(*RefID, Val->getType());

    jeandle::PEAResult::Effect E;
    E.Kind = jeandle::PEAResult::EffectKind::EliminateStore;
    E.Block = SI->getParent();
    E.Target = SI;
    E.SeqNo = Result.nextSeqNo();
    E.ObjID = *BaseID;
    Result.addBlockEffect(std::move(E));
    return;
  }
  FieldStates[*BaseID][*Offset] = jeandle::FieldValue::scalar(Val);

  jeandle::PEAResult::Effect E;
  E.Kind = jeandle::PEAResult::EffectKind::EliminateStore;
  E.Block = SI->getParent();
  E.Target = SI;
  E.SeqNo = Result.nextSeqNo();
  E.ObjID = *BaseID;
  Result.addBlockEffect(std::move(E));
}

void Analyzer::tier2Load(LoadInst *LI) {
  Value *Ptr = LI->getPointerOperand();

  auto BaseID = jeandle::pea::resolveVirtualRef(Ptr, CurrentState, Aliases, DL);
  if (!BaseID)
    return;

  auto Offset = jeandle::pea::resolveFieldOffset(Ptr, DL);
  if (!Offset) {
    Eligible[*BaseID] = false;
    return;
  }

  Type *LoadTy = LI->getType();

  const jeandle::FieldValue *Existing = nullptr;
  auto It = FieldStates.find(*BaseID);
  if (It != FieldStates.end()) {
    auto It2 = It->second.find(*Offset);
    if (It2 != It->second.end())
      Existing = &It2->second;
  }

  if (!Existing || Existing->isUnknown()) {
    // Default value for a never-written field.
    Constant *Def = jeandle::FieldValue::defaultFor(LoadTy);
    jeandle::PEAResult::Effect E;
    E.Kind = jeandle::PEAResult::EffectKind::ReplaceLoad;
    E.Block = LI->getParent();
    E.Target = LI;
    E.Replacement = Def;
    E.SeqNo = Result.nextSeqNo();
    E.ObjID = *BaseID;
    Result.addBlockEffect(std::move(E));
    Aliases.addScalarAlias(LI, Def);
    return;
  }

  if (Existing->isScalar()) {
    Value *V = Existing->getScalar();
    // Same-bit-width primitive↔primitive type mismatch → emit a bitcast
    // (e.g. i32-stored / float-loaded). Pointer↔primitive (or cross-AS
    // pointer↔pointer, or different bit widths) bails to ineligible per
    // invariant F2 in PartialEscape.h.
    Value *Coerced = coerceToType(V, LoadTy, LI);
    if (!Coerced) {
      Eligible[*BaseID] = false;
      return;
    }
    jeandle::PEAResult::Effect E;
    E.Kind = jeandle::PEAResult::EffectKind::ReplaceLoad;
    E.Block = LI->getParent();
    E.Target = LI;
    E.Replacement = Coerced;
    E.SeqNo = Result.nextSeqNo();
    E.ObjID = *BaseID;
    Result.addBlockEffect(std::move(E));
    Aliases.addScalarAlias(LI, Coerced);
    return;
  }

  if (Existing->isVirtualRef()) {
    // Nested-virtual load — current limitation: mark both ineligible.
    Eligible[*BaseID] = false;
    Eligible[Existing->getVirtualRef()] = false;
    return;
  }

  if (Existing->isMaterializedRef()) {
    // A virtual's reference field carries a materialized pointer (e.g.
    // produced by a field-PHI synthesis at a merge block). Forward the load
    // to the materialized value, matching the Scalar handler.
    Value *V = Existing->getMaterialized();
    if (!V) {
      Eligible[*BaseID] = false;
      return;
    }
    // A materialized ref slot can only be loaded back as a pointer (and in
    // practice, since LLVM 17 uses opaque pointers, only as the same
    // ptr-AS). coerceToType bails on ptr↔primitive (F2) and on cross-AS
    // pointer pairs.
    Value *Coerced = coerceToType(V, LoadTy, LI);
    if (!Coerced) {
      Eligible[*BaseID] = false;
      return;
    }
    jeandle::PEAResult::Effect E;
    E.Kind = jeandle::PEAResult::EffectKind::ReplaceLoad;
    E.Block = LI->getParent();
    E.Target = LI;
    E.Replacement = Coerced;
    E.SeqNo = Result.nextSeqNo();
    E.ObjID = *BaseID;
    Result.addBlockEffect(std::move(E));
    Aliases.addScalarAlias(LI, Coerced);
    return;
  }

  // Should be unreachable; FieldValue::Tag is a closed enum.
  Eligible[*BaseID] = false;
}

// ---------------------------------------------------------------------------
// Tier-2 JavaOp folding on virtual receivers.
// ---------------------------------------------------------------------------

void Analyzer::emitReplaceCall(CallBase *CB, Value *Replacement,
                               jeandle::ObjectID ID) {
  jeandle::PEAResult::Effect E;
  E.Kind = jeandle::PEAResult::EffectKind::ReplaceCall;
  E.Block = CB->getParent();
  E.Target = CB;
  E.Replacement = Replacement;
  E.SeqNo = Result.nextSeqNo();
  E.ObjID = ID;
  Result.addBlockEffect(std::move(E));
  // Scalar-alias the call's value so downstream resolveVirtualRef queries
  // (e.g. another JavaOp later in the same block whose operand is the call
  // result) see through to the replacement constant.
  Aliases.addScalarAlias(CB, Replacement);
}

std::optional<bool> Analyzer::evalSubtypeRelation(uintptr_t SubKlass,
                                                  uintptr_t SuperKlass) {
  const jeandle::VMCallbacks *CB = jeandle::getVMCallbacks();
  if (!CB || !CB->IsSubtype || !CB->IsInterface)
    return std::nullopt;
  if (CB->IsSubtype(SubKlass, SuperKlass))
    return true;
  // Virtual objects always have an exact, concrete klass (we know the
  // allocation site). Pass Exact=true to areKlassesIncompatible.
  if (jeandle::areKlassesIncompatible(SubKlass, /*Exact=*/true, SuperKlass))
    return false;
  return std::nullopt;
}

bool Analyzer::foldArrayLength(CallBase *CB) {
  if (CB->arg_size() < 1)
    return false;
  auto BaseID = jeandle::pea::resolveVirtualRef(CB->getArgOperand(0),
                                                CurrentState, Aliases, DL);
  if (!BaseID)
    return false;
  jeandle::VirtualObject &VObj = *Result.VirtualObjects[*BaseID];
  if (!VObj.isArray())
    return false;
  Type *I32 = Type::getInt32Ty(F.getContext());
  Constant *Len = ConstantInt::get(I32, VObj.ArrayLength);
  emitReplaceCall(CB, Len, *BaseID);
  return true;
}

bool Analyzer::foldLoadKlass(CallBase *CB) {
  if (CB->arg_size() < 1)
    return false;
  auto BaseID = jeandle::pea::resolveVirtualRef(CB->getArgOperand(0),
                                                CurrentState, Aliases, DL);
  if (!BaseID)
    return false;
  jeandle::VirtualObject &VObj = *Result.VirtualObjects[*BaseID];
  if (VObj.Klass == 0)
    return false;
  LLVMContext &Ctx = F.getContext();
  Type *I64 = Type::getInt64Ty(Ctx);
  Type *PtrTy = PointerType::get(Ctx, jeandle::AddrSpace::CHeapAddrSpace);
  Constant *KlassAsInt = ConstantInt::get(I64, VObj.Klass);
  Constant *KlassPtr = ConstantExpr::getIntToPtr(KlassAsInt, PtrTy);
  emitReplaceCall(CB, KlassPtr, *BaseID);
  return true;
}

bool Analyzer::foldCheckCast(CallBase *CB) {
  if (CB->arg_size() < 2)
    return false;
  uintptr_t SuperK = jeandle::extractKlassConstant(CB->getArgOperand(0));
  if (SuperK == 0)
    return false;
  auto BaseID = jeandle::pea::resolveVirtualRef(CB->getArgOperand(1),
                                                CurrentState, Aliases, DL);
  if (!BaseID)
    return false;
  jeandle::VirtualObject &VObj = *Result.VirtualObjects[*BaseID];
  if (VObj.Klass == 0)
    return false;
  auto Folded = evalSubtypeRelation(VObj.Klass, SuperK);
  if (!Folded)
    return false;
  Constant *Res = *Folded ? ConstantInt::getTrue(CB->getType())
                          : ConstantInt::getFalse(CB->getType());
  emitReplaceCall(CB, Res, *BaseID);
  return true;
}

bool Analyzer::foldCheckInstanceOf(CallBase *CB) {
  // Same shape as foldCheckCast: (super_klass, oop) -> i1.
  return foldCheckCast(CB);
}

bool Analyzer::foldInstanceOf(CallBase *CB) {
  if (CB->arg_size() < 2)
    return false;
  uintptr_t SuperK = jeandle::extractKlassConstant(CB->getArgOperand(0));
  if (SuperK == 0)
    return false;
  auto BaseID = jeandle::pea::resolveVirtualRef(CB->getArgOperand(1),
                                                CurrentState, Aliases, DL);
  if (!BaseID)
    return false;
  jeandle::VirtualObject &VObj = *Result.VirtualObjects[*BaseID];
  if (VObj.Klass == 0)
    return false;
  // Virtual objects are non-null by construction, so we can collapse the
  // null-check inside instanceof and reduce to a pure subtype query.
  auto Folded = evalSubtypeRelation(VObj.Klass, SuperK);
  if (!Folded)
    return false;
  Type *I32 = Type::getInt32Ty(F.getContext());
  Constant *Res = ConstantInt::get(I32, *Folded ? 1 : 0);
  emitReplaceCall(CB, Res, *BaseID);
  return true;
}

bool Analyzer::foldMonitorEnter(CallBase *CB) {
  if (CB->arg_size() < 1)
    return false;
  auto BaseID = jeandle::pea::resolveVirtualRef(CB->getArgOperand(0),
                                                CurrentState, Aliases, DL);
  if (!BaseID)
    return false;
  // Lock confinement: the lock counter is balanced per-block at commit
  // time. A monitorenter on a virtual is always safe to provisionally
  // elide; if the matching monitorexit is missing, commit() will flip the
  // virtual to ineligible and the effects will be dropped.
  ++LockCounts[*BaseID];
  // Push the elided enter onto the live stack so materializeAt can undo
  // only the unbalanced enters along this path if the object later escapes.
  LiveLockEnters[*BaseID].push_back(CB);
  Constant *True = ConstantInt::getTrue(CB->getType());
  emitReplaceCall(CB, True, *BaseID);
  return true;
}

bool Analyzer::foldMonitorExit(CallBase *CB) {
  if (CB->arg_size() < 1)
    return false;
  auto BaseID = jeandle::pea::resolveVirtualRef(CB->getArgOperand(0),
                                                CurrentState, Aliases, DL);
  if (!BaseID)
    return false;
  auto It = LockCounts.find(*BaseID);
  if (It == LockCounts.end() || It->second == 0) {
    // Unbalanced monitorexit (release without acquire on this virtual). Mark
    // the virtual ineligible and let the generic escape path keep the call.
    Eligible[*BaseID] = false;
    return false;
  }
  --It->second;
  // Pop the matching enter off the live stack. The pair is now balanced
  // and both calls' ReplaceCall(true) effects stay in place — the eventual
  // dead-code sweep erases them along with the original allocation.
  auto SIt = LiveLockEnters.find(*BaseID);
  assert(SIt != LiveLockEnters.end() && !SIt->second.empty() &&
         "live stack must be non-empty when LockCount > 0");
  SIt->second.pop_back();
  Constant *True = ConstantInt::getTrue(CB->getType());
  emitReplaceCall(CB, True, *BaseID);
  return true;
}

bool Analyzer::foldArrayStoreCheck(CallBase *CB) {
  // jeandle.array_store_check(value, array). Section 2.3.14 of the PEA plan:
  // this op is read-only on the heap, so it never causes an escape by itself.
  // We always return true here (we handled the call) — folding to a constant
  // is best-effort. Current limitation: without a VMCallback giving us the
  // array element klass, we can only fold when both array and value are
  // virtuals whose klasses we know and IsSubtype confirms compatibility.
  if (CB->arg_size() < 2)
    return true; // malformed; treat as non-escaping no-op
  auto ArrayID = jeandle::pea::resolveVirtualRef(CB->getArgOperand(1),
                                                 CurrentState, Aliases, DL);
  if (!ArrayID)
    return true;
  auto ValueID = jeandle::pea::resolveVirtualRef(CB->getArgOperand(0),
                                                 CurrentState, Aliases, DL);
  if (!ValueID)
    return true; // can't fold, but the array doesn't escape
  jeandle::VirtualObject &ArrayObj = *Result.VirtualObjects[*ArrayID];
  jeandle::VirtualObject &ValueObj = *Result.VirtualObjects[*ValueID];
  // TODO: use a VMCallback that returns the array's element klass; for now
  // we compare against the array's own klass which is conservative.
  const jeandle::VMCallbacks *VMCB = jeandle::getVMCallbacks();
  if (!VMCB || !VMCB->IsSubtype)
    return true;
  if (ArrayObj.Klass == 0 || ValueObj.Klass == 0)
    return true;
  if (VMCB->IsSubtype(ValueObj.Klass, ArrayObj.Klass)) {
    Constant *True = ConstantInt::getTrue(CB->getType());
    emitReplaceCall(CB, True, *ArrayID);
  }
  return true;
}

bool Analyzer::foldCheckIfValueBased(CallBase *CB) {
  // Current limitation: no VMCallback exists yet to answer the "is this
  // klass a value-based class?" query. Without it we cannot prove that a
  // virtual receiver is safe for value-based-warning elision, so we
  // conservatively return false and let the generic escape path mark the
  // base ineligible. The end result is that any virtual that passes through
  // a value-based check stays allocated.
  // TODO: add an IsValueBased VM callback and fold here.
  (void)CB;
  return false;
}

bool Analyzer::tier2JavaOpFold(CallBase *CB) {
  using namespace jeandle::pea;
  if (isJeandleArrayLength(CB))       return foldArrayLength(CB);
  if (isJeandleLoadKlass(CB))         return foldLoadKlass(CB);
  if (isJeandleCheckCast(CB))         return foldCheckCast(CB);
  if (isJeandleCheckInstanceOf(CB))   return foldCheckInstanceOf(CB);
  if (isJeandleInstanceOf(CB))        return foldInstanceOf(CB);
  if (isJeandleMonitorEnter(CB))      return foldMonitorEnter(CB);
  if (isJeandleMonitorExit(CB))       return foldMonitorExit(CB);
  if (isJeandleArrayStoreCheck(CB))   return foldArrayStoreCheck(CB);
  if (isJeandleCheckIfValueBased(CB)) return foldCheckIfValueBased(CB);
  return false;
}

void Analyzer::propagatePointerAlias(Instruction *I) {
  // The instruction is a pointer-identity-preserving transformation whose
  // operand carries a virtual alias. Forward the alias to the result.
  if (Aliases.getVirtualAlias(I))
    return;
  auto BaseID =
      jeandle::pea::resolveVirtualRef(I, CurrentState, Aliases, DL);
  if (!BaseID) {
    // Couldn't resolve — the underlying chain may have already escaped.
    materializeAllVirtualOperands(I);
    return;
  }
  Aliases.addVirtualAlias(I, *BaseID);
}

void Analyzer::materializeAllVirtualOperands(Instruction *I) {
  // Trigger materialization for every distinct virtual ObjectID that I uses.
  // After materializeAt, the per-object state in CurrentState flips to
  // Materialized so subsequent resolveVirtualRef queries return nullopt.
  // Operand-input rewriting is handled implicitly by the transform: it RAUWs
  // the original allocation with the materialized CallInst, so all IR uses
  // (including I's operand here) auto-update.
  SmallVector<jeandle::ObjectID, 4> ToMaterialize;
  DenseSet<jeandle::ObjectID> Seen;
  for (Use &U : I->operands()) {
    Value *V = U.get();
    if (!V)
      continue;
    if (auto MaybeID =
            jeandle::pea::resolveVirtualRef(V, CurrentState, Aliases, DL)) {
      if (Seen.insert(*MaybeID).second)
        ToMaterialize.push_back(*MaybeID);
    }
  }
  llvm::sort(ToMaterialize);
  for (jeandle::ObjectID ID : ToMaterialize)
    materializeAt(ID, I);
}

// Compute the SAFE materialization point for an allocation: the earliest IR
// position immediately after the allocation completes. The new materialization
// invoke must be inserted here so that it dominates every existing use of the
// original allocation; otherwise the RAUW that snaps OrigAlloc onto the new
// invoke would replace earlier-in-block uses with a later-defined value, an
// SSA dominance violation that downstream passes (e.g. RewriteStatepointsForGC)
// reject.
//
// For an InvokeInst allocation, "immediately after" is the first non-PHI/dbg
// instruction in the normal-dest block. For a plain CallInst allocation, it's
// the instruction directly following the call. The returned instruction is
// guaranteed to dominate every use of the original allocation in the function.
static Instruction *computeMaterializationPoint(llvm::CallBase *Alloc) {
  if (auto *II = llvm::dyn_cast<llvm::InvokeInst>(Alloc)) {
    llvm::BasicBlock *Normal = II->getNormalDest();
    return &*Normal->getFirstNonPHIOrDbg();
  }
  return Alloc->getNextNode();
}

void Analyzer::materializeAt(jeandle::ObjectID ID,
                             Instruction *InsertBefore) {
  if (Materialized.count(ID))
    return; // idempotent — first escape wins; also breaks nested-cycles.
  if (!Eligible.lookup(ID))
    return; // already gave up on this object; nothing to materialize.

  jeandle::VirtualObject &VObj = *Result.VirtualObjects[ID];

  // Cycle prevention: insert into Materialized BEFORE recursing on any
  // nested VirtualRef. Mirrors Graal's "flip the state then recurse" order in
  // PartialEscapeBlockState.materializeWithCommit. This guarantees that a
  // self-referential or cyclic field graph (A.f = B, B.g = A) terminates: the
  // recursive call short-circuits via the Materialized check at the top.
  // The cascade for strict-lock-order (below) also relies on this happening
  // before recursion so it doesn't re-enter the same ID.
  Materialized.insert(ID);

  // Lock cascade on materialization. When the object has live locks at
  // this point (LockCount > 0) AND the target's runtime requires strict
  // lock nesting (HotSpot LM_LIGHTWEIGHT), every other virtual that is
  // still holding locks must be materialized at the same insertion point
  // so that the runtime's monitor-stack ordering remains intact. We don't
  // track per-object max lock depth, so we conservatively cascade every
  // other still-locked virtual (sound but may over-materialize).
  auto LCIt = LockCounts.find(ID);
  bool HasLiveLocks = (LCIt != LockCounts.end() && LCIt->second != 0);
  if (HasLiveLocks && JeandleAssumeStrictLockOrder) {
    SmallVector<jeandle::ObjectID, 4> Cascade;
    for (auto &Kv : LockCounts) {
      if (Kv.first == ID || Kv.second == 0)
        continue;
      if (Materialized.count(Kv.first))
        continue;
      Cascade.push_back(Kv.first);
    }
    llvm::sort(Cascade); // deterministic order
    for (jeandle::ObjectID OtherID : Cascade)
      materializeAt(OtherID, InsertBefore);
  }

  // Undo monitor-call elisions for ID when there are live locks. For each
  // unbalanced enter on the live stack we (a) drop its previously-recorded
  // ReplaceCall(true) effect so the original call survives the transform,
  // and (b) emit a ReplaceInput effect that retargets the call's first
  // operand (the receiver) onto the materialized pointer at apply time. The
  // transform's NewAllocFor map redirects VObj.AllocationCall onto the live
  // materialized invoke. Matching exits downstream of the escape point are
  // not elided in the first place (foldMonitorExit on a Materialized object
  // returns false) and survive in IR with their operand RAUW'd.
  //
  // Balanced pairs whose exits we already saw on this path are NOT on the
  // live stack — their elision stays in place (better performance).
  auto SIt = LiveLockEnters.find(ID);
  if (HasLiveLocks && SIt != LiveLockEnters.end() && !SIt->second.empty()) {
    // Snapshot the live stack so we can clear it before recursing.
    SmallVector<CallBase *, 4> Stack(SIt->second.begin(), SIt->second.end());
    DenseSet<Instruction *> ToUndo(Stack.begin(), Stack.end());

    // Walk every block's effect list and erase any ReplaceCall effect whose
    // target is one of the unbalanced enter call sites.
    for (auto &Kv : Result.BlockEffects) {
      auto &Effects = Kv.second;
      Effects.erase(
          std::remove_if(Effects.begin(), Effects.end(),
                         [&](const jeandle::PEAResult::Effect &E) {
                           return E.Kind ==
                                      jeandle::PEAResult::EffectKind::ReplaceCall &&
                                  ToUndo.count(E.Target);
                         }),
          Effects.end());
    }

    // Emit a ReplaceInput effect per enter, retargeting op0 onto the
    // materialized pointer (resolved via NewAllocFor at apply time).
    for (CallBase *Call : Stack) {
      jeandle::PEAResult::Effect E;
      E.Kind = jeandle::PEAResult::EffectKind::ReplaceInput;
      E.Block = Call->getParent();
      E.SeqNo = Result.nextSeqNo();
      E.Target = Call;
      E.InputIndex = 0;
      E.Replacement = VObj.AllocationCall;
      E.ObjID = ID;
      Result.addBlockEffect(std::move(E));
    }

    // The locks are now satisfied by real IR calls; clear the live stack
    // and the lock counter so commit()'s post-pass doesn't flip the object
    // back to ineligible.
    LockCounts[ID] = 0;
    LiveLockEnters.erase(ID);
  }

  // Recursive prerequisite materialization. For each field that holds a
  // VirtualRef to an inner virtual object, materialize the inner first, then
  // rewrite the outer's FieldStates entry to a MaterializedRef pointing at
  // the inner's *original* allocation. The transform substitutes that with
  // the new materialized invoke at apply time via NewAllocFor.
  {
    auto FSIt = FieldStates.find(ID);
    if (FSIt != FieldStates.end()) {
      SmallVector<int64_t, 4> NestedOffsets;
      for (auto &Kv : FSIt->second)
        if (Kv.second.isVirtualRef())
          NestedOffsets.push_back(Kv.first);
      llvm::sort(NestedOffsets); // determinism
      for (int64_t Off : NestedOffsets) {
        // Re-lookup each iteration; the recursion may have mutated FieldStates
        // (updateStatesForMaterialized below) and invalidated iterators.
        auto It2 = FieldStates.find(ID);
        if (It2 == FieldStates.end())
          break;
        auto OffIt = It2->second.find(Off);
        if (OffIt == It2->second.end() || !OffIt->second.isVirtualRef())
          continue;
        jeandle::ObjectID InnerID = OffIt->second.getVirtualRef();
        materializeAt(InnerID, InsertBefore);
        if (!Eligible.lookup(InnerID)) {
          // Inner gave up — the outer cannot be materialized either, since
          // its replay would need a value inner can no longer produce.
          Eligible[ID] = false;
          return;
        }
        jeandle::VirtualObject &InnerVO = *Result.VirtualObjects[InnerID];
        FieldStates[ID][Off] =
            jeandle::FieldValue::materializedRef(InnerVO.AllocationCall);

        // updateStatesForMaterialized: every other still-tracked object whose
        // FieldStates references InnerID must also be flipped to
        // MaterializedRef so any later store/load on those objects sees the
        // materialized pointer. This mirrors Graal's
        // updateStatesForMaterialized / ensureMaterialized helpers.
        for (auto &OtherKv : FieldStates) {
          if (OtherKv.first == ID)
            continue;
          for (auto &OtherEntry : OtherKv.second) {
            if (OtherEntry.second.isVirtualRef() &&
                OtherEntry.second.getVirtualRef() == InnerID) {
              OtherEntry.second = jeandle::FieldValue::materializedRef(
                  InnerVO.AllocationCall);
            }
          }
        }
        // Drop alias-map entries that resolve to InnerID. The transform's
        // RAUW propagates the new pointer through the IR for downstream
        // consumers; we no longer want the analyzer to treat those Values as
        // virtual aliases of InnerID.
        SmallVector<Value *, 4> AliasesToDrop;
        for (auto &AKv : Aliases.virtualAliasesView()) {
          if (AKv.second == InnerID)
            AliasesToDrop.push_back(AKv.first);
        }
        for (Value *V : AliasesToDrop)
          Aliases.resetAlias(V);
      }
    }
  }

  // Soundness fix: rather than insert the materialization invoke at the
  // escape point (which would leave SSA-uses in the same block ahead of the
  // new definition once the transform RAUWs OrigAlloc onto it), hoist the
  // materialization back to the original allocation point. The chosen SafeIP
  // immediately follows OrigAlloc, so the new materialization invoke
  // dominates every existing use of OrigAlloc in the function.
  Instruction *SafeIP = computeMaterializationPoint(VObj.AllocationCall);
  assert(SafeIP && "alloc must have a follow-on instruction at SafeIP");

  // Per-field dominance check. After VirtualRef rewriting above, all entries
  // are Scalar or MaterializedRef. For MaterializedRef values produced by the
  // recursion (= an inner object's original allocation Instruction), the
  // dominance is trivially satisfied: the inner's allocation precedes the
  // store-into-outer in IR order, hence dominates outer's SafeIP.
  auto FSIt = FieldStates.find(ID);
  if (FSIt != FieldStates.end()) {
    for (auto &Kv : FSIt->second) {
      const jeandle::FieldValue &FV = Kv.second;
      Value *V = nullptr;
      if (FV.isScalar())
        V = FV.getScalar();
      else if (FV.isMaterializedRef())
        V = FV.getMaterialized();
      else
        continue;
      if (!V)
        continue;
      if (auto *VI = dyn_cast<Instruction>(V)) {
        if (!DT.dominates(VI, SafeIP)) {
          Eligible[ID] = false;
          return;
        }
      }
    }
  }

  // Snapshot tracked field values at this escape point. The transform replays
  // them as stores immediately after the new allocation call.
  jeandle::PEAResult::Effect E;
  E.Kind = jeandle::PEAResult::EffectKind::Materialize;
  E.Block = SafeIP->getParent();
  E.SeqNo = Result.nextSeqNo();
  E.InsertBefore = SafeIP;
  // Carry the original allocation as Target so the transform can RAUW it onto
  // the new materialized CallInst.
  E.Target = VObj.AllocationCall;
  E.ObjID = ID;

  // Copy a "deopt" operand bundle onto the materialization invoke.
  E.DeoptBundleSource = VObj.AllocationCall;

  if (FSIt != FieldStates.end()) {
    // Deterministic ordering for the snapshot.
    SmallVector<int64_t, 8> Offsets;
    Offsets.reserve(FSIt->second.size());
    for (auto &Kv : FSIt->second)
      Offsets.push_back(Kv.first);
    llvm::sort(Offsets);
    for (int64_t Off : Offsets) {
      const jeandle::FieldValue &FV = FSIt->second.lookup(Off);
      if (FV.isUnknown())
        continue;
      E.FieldEntries.push_back({Off, FV});
    }
  }
  Result.addBlockEffect(std::move(E));

  // Flip the per-object state so the rest of the analyzer treats the object
  // as already-materialized on this path. We hand the original allocation in
  // as a placeholder pointer; the transform RAUWs it onto the real
  // materialized CallInst before erasing it.
  CurrentState.getObjectStateForModification(ID).materialize(
      VObj.AllocationCall);
}

void Analyzer::dropEffectsFor(jeandle::ObjectID ID) {
  bool DroppedAllocation = false;
  for (auto &Kv : Result.BlockEffects) {
    auto &Effects = Kv.second;
    Effects.erase(
        std::remove_if(Effects.begin(), Effects.end(),
                       [&](const jeandle::PEAResult::Effect &E) {
                         if (E.ObjID != ID)
                           return false;
                         if (E.Kind ==
                             jeandle::PEAResult::EffectKind::EliminateAllocation)
                           DroppedAllocation = true;
                         return true;
                       }),
        Effects.end());
  }
  if (DroppedAllocation) {
    --Result.VirtualizationDelta;
    ++Result.AllocationDelta;
  }
  Result.EscapeClassification[ID] =
      jeandle::PEAResult::EscapeKind::AlwaysEscapes;
}

void Analyzer::commit() {
  // Any virtual whose monitor lock count is non-zero at the end of analysis
  // has unbalanced enter/exit pairs (e.g., only an enter was seen, or the
  // matching exit lives in a different block we don't track yet). Mark
  // those ineligible up-front; the per-object loop below will drop their
  // effects.
  for (auto &Kv : LockCounts) {
    if (Kv.second != 0)
      Eligible[Kv.first] = false;
  }

  // Iterate by dense ObjectID order for determinism. The only remaining
  // post-pass cleanup is dropping effects for objects that became
  // ineligible during the walk (lock imbalance above; nested-virtual
  // discovery; tier-2 type mismatch / non-const offset; etc.). Cross-block
  // escapes no longer disqualify an object — they trigger materialization.
  for (auto &VObjUP : Result.VirtualObjects) {
    jeandle::ObjectID ID = VObjUP->getID();
    auto EIt = Eligible.find(ID);
    bool IsEligible = (EIt != Eligible.end()) && EIt->second;
    if (!IsEligible)
      dropEffectsFor(ID);
  }
}

// Preorder (outer-first) collection of all loops reachable from L. Determinism
// follows LoopInfo::iterator / Loop::getSubLoops ordering, which mirrors the
// source-IR block order.
static void collectLoopsPreorder(Loop *L, SmallVectorImpl<Loop *> &Out) {
  Out.push_back(L);
  for (Loop *Sub : L->getSubLoops())
    collectLoopsPreorder(Sub, Out);
}

// Loop soundness: after the RPO walk has populated BlockExits but before
// commit(), force-materialize every virtual that is still virtual at any
// loop preheader's terminator. This makes loops trivially sound — no
// virtual survives the loop boundary — at the cost of giving up
// virtualization across loops. Combined with the tier1Allocate refusal of
// loop-body allocs, the analyzer never tracks an object across a back-edge.
//
// Important sequencing: this MUST run after the per-block analysis (we need
// BlockExits[preheader] to know what's still virtual on the way into the loop)
// and BEFORE commit() (so the Materialize effects we add are subject to the
// same eligibility filter that drops effects for objects we've decided to
// abandon).
void Analyzer::materializeBeforeLoops() {
  SmallVector<Loop *, 8> AllLoops;
  for (Loop *L : LI)
    collectLoopsPreorder(L, AllLoops);

  for (Loop *L : AllLoops) {
    BasicBlock *PH = L->getLoopPreheader();
    if (!PH) {
      // Unsimplified / irreducible loop: skip. The conservative fallback
      // would be to materialize at every header predecessor, but Jeandle
      // runs LoopSimplify before PEA so this branch should be unreachable
      // in production. TODO: handle missing-preheader case.
      continue;
    }
    auto It = BlockExits.find(PH);
    if (It == BlockExits.end())
      continue;
    BlockExitInfo &PHExit = It->second;

    // Snapshot+sort the virtual IDs to materialize. Sorting by ObjectID gives
    // deterministic effect ordering across runs.
    SmallVector<jeandle::ObjectID, 4> Vs(PHExit.Virtuals.begin(),
                                          PHExit.Virtuals.end());
    llvm::sort(Vs);
    for (jeandle::ObjectID ID : Vs) {
      if (!Eligible.lookup(ID))
        continue;
      materializeAtPredFromExitInfo(ID, PH, PHExit);
    }
  }
}

// Like materializeAt, but operates against a pred's BlockExitInfo snapshot
// rather than the analyzer's current per-block state (which has moved on by
// the time materializeBeforeLoops runs). The function-wide MaterializedAtPred
// map dedups (and breaks cycles between) recursive nested-virtual
// materializations within a single PH and across multiple call sites
// (e.g. a mixed-state merge and a loop-preheader sweep at the same PH).
void Analyzer::materializeAtPredFromExitInfo(
    jeandle::ObjectID ID, BasicBlock *PH, BlockExitInfo &ExitInfo) {
  auto &MatInPH = MaterializedAtPred[PH];
  if (MatInPH.count(ID))
    return;
  if (!Eligible.lookup(ID))
    return;

  jeandle::VirtualObject &VObj = *Result.VirtualObjects[ID];

  // Cycle prevention before recursion (mirrors materializeAt).
  MatInPH.insert(ID);

  // Strict-lock-order cascade against this pred's snapshot. If any other
  // virtual is still holding locks on this path, materialize it first so the
  // runtime's monitor-stack ordering is preserved.
  auto LCIt = ExitInfo.LockCounts.find(ID);
  bool HasLiveLocks = (LCIt != ExitInfo.LockCounts.end() && LCIt->second != 0);
  if (HasLiveLocks && JeandleAssumeStrictLockOrder) {
    SmallVector<jeandle::ObjectID, 4> Cascade;
    for (auto &Kv : ExitInfo.LockCounts) {
      if (Kv.first == ID || Kv.second == 0)
        continue;
      if (MatInPH.count(Kv.first))
        continue;
      Cascade.push_back(Kv.first);
    }
    llvm::sort(Cascade);
    for (jeandle::ObjectID OtherID : Cascade)
      materializeAtPredFromExitInfo(OtherID, PH, ExitInfo);
  }

  // Undo monitor-call elisions for ID when there are live locks. Reads the
  // snapshot's LiveLockEnters (the unbalanced enter call sites along the
  // path that produced ExitInfo). Mirrors materializeAt's logic.
  auto SIt = ExitInfo.LiveLockEnters.find(ID);
  if (HasLiveLocks && SIt != ExitInfo.LiveLockEnters.end() &&
      !SIt->second.empty()) {
    SmallVector<CallBase *, 4> Stack(SIt->second.begin(), SIt->second.end());
    DenseSet<Instruction *> ToUndo(Stack.begin(), Stack.end());
    for (auto &Kv : Result.BlockEffects) {
      auto &Effects = Kv.second;
      Effects.erase(
          std::remove_if(Effects.begin(), Effects.end(),
                         [&](const jeandle::PEAResult::Effect &E) {
                           return E.Kind ==
                                      jeandle::PEAResult::EffectKind::ReplaceCall &&
                                  ToUndo.count(E.Target);
                         }),
          Effects.end());
    }
    for (CallBase *Call : Stack) {
      jeandle::PEAResult::Effect E;
      E.Kind = jeandle::PEAResult::EffectKind::ReplaceInput;
      E.Block = Call->getParent();
      E.SeqNo = Result.nextSeqNo();
      E.Target = Call;
      E.InputIndex = 0;
      E.Replacement = VObj.AllocationCall;
      E.ObjID = ID;
      Result.addBlockEffect(std::move(E));
    }
    // Clear locks and stack in both the live state (for commit()) and the
    // snapshot (for any subsequent merge that reads from this pred).
    LockCounts[ID] = 0;
    LiveLockEnters.erase(ID);
    ExitInfo.LockCounts.erase(ID);
    ExitInfo.LiveLockEnters.erase(ID);
  }

  // Recursive prerequisite materialization, mirroring materializeAt but
  // walking ExitInfo.FieldStates instead of the analyzer's live FieldStates.
  {
    auto FSIt = ExitInfo.FieldStates.find(ID);
    if (FSIt != ExitInfo.FieldStates.end()) {
      SmallVector<int64_t, 4> NestedOffsets;
      for (auto &Kv : FSIt->second)
        if (Kv.second.isVirtualRef())
          NestedOffsets.push_back(Kv.first);
      llvm::sort(NestedOffsets);
      for (int64_t Off : NestedOffsets) {
        auto It2 = ExitInfo.FieldStates.find(ID);
        if (It2 == ExitInfo.FieldStates.end())
          break;
        auto OffIt = It2->second.find(Off);
        if (OffIt == It2->second.end() || !OffIt->second.isVirtualRef())
          continue;
        jeandle::ObjectID InnerID = OffIt->second.getVirtualRef();
        materializeAtPredFromExitInfo(InnerID, PH, ExitInfo);
        if (!Eligible.lookup(InnerID)) {
          Eligible[ID] = false;
          return;
        }
        jeandle::VirtualObject &InnerVO = *Result.VirtualObjects[InnerID];
        ExitInfo.FieldStates[ID][Off] =
            jeandle::FieldValue::materializedRef(InnerVO.AllocationCall);
        // updateStatesForMaterialized in the snapshot.
        for (auto &OtherKv : ExitInfo.FieldStates) {
          if (OtherKv.first == ID)
            continue;
          for (auto &E : OtherKv.second) {
            if (E.second.isVirtualRef() &&
                E.second.getVirtualRef() == InnerID) {
              E.second = jeandle::FieldValue::materializedRef(
                  InnerVO.AllocationCall);
            }
          }
        }
      }
    }
  }

  // Soundness fix (mirrors materializeAt).
  Instruction *SafeIP = &*PH->getFirstNonPHIOrDbg();
  Instruction *AllocSafe = computeMaterializationPoint(VObj.AllocationCall);
  if (AllocSafe && AllocSafe->getParent() == PH &&
      !DT.dominates(AllocSafe, SafeIP)) {
    SafeIP = AllocSafe;
  }

  // Per-field dominance check.
  auto FSIt = ExitInfo.FieldStates.find(ID);
  if (FSIt != ExitInfo.FieldStates.end()) {
    for (auto &Kv : FSIt->second) {
      const jeandle::FieldValue &FV = Kv.second;
      Value *V = nullptr;
      if (FV.isScalar())
        V = FV.getScalar();
      else if (FV.isMaterializedRef())
        V = FV.getMaterialized();
      else
        continue;
      if (!V)
        continue;
      if (auto *VI = dyn_cast<Instruction>(V)) {
        if (!DT.dominates(VI, SafeIP)) {
          Eligible[ID] = false;
          return;
        }
      }
    }
  }

  jeandle::PEAResult::Effect E;
  E.Kind = jeandle::PEAResult::EffectKind::Materialize;
  E.Block = SafeIP->getParent();
  E.SeqNo = Result.nextSeqNo();
  E.InsertBefore = SafeIP;
  E.Target = VObj.AllocationCall;
  E.ObjID = ID;
  E.DeoptBundleSource = VObj.AllocationCall;

  if (FSIt != ExitInfo.FieldStates.end()) {
    SmallVector<int64_t, 8> Offsets;
    Offsets.reserve(FSIt->second.size());
    for (auto &Kv : FSIt->second)
      Offsets.push_back(Kv.first);
    llvm::sort(Offsets);
    for (int64_t Off : Offsets) {
      const jeandle::FieldValue &FV = FSIt->second.lookup(Off);
      if (FV.isUnknown())
        continue;
      E.FieldEntries.push_back({Off, FV});
    }
  }
  Result.addBlockEffect(std::move(E));

  // Post-materialize, flip the snapshot's per-object state so any later
  // merge that reads from this pred's ExitInfo sees the object as
  // materialized. The placeholder MaterializedValue is OrigAlloc; the
  // transform's MatPerBlock substitutes the live NewInv at apply time.
  ExitInfo.Virtuals.erase(ID);
  ExitInfo.Materialized.insert(ID);
  ExitInfo.MaterializedValues[ID] = VObj.AllocationCall;
  ExitInfo.FieldStates.erase(ID);
  ExitInfo.LockCounts.erase(ID);
}

jeandle::PEAResult Analyzer::run() {
  ReversePostOrderTraversal<Function *> RPOT(&F);
  for (BasicBlock *BB : RPOT)
    processBlock(BB);
  // Force-materialize at every loop preheader before commit() so no virtual
  // survives into a loop body.
  materializeBeforeLoops();
  commit();
  return std::move(Result);
}

} // anonymous namespace

PartialEscapeAnalysis::Result
PartialEscapeAnalysis::run(Function &F, FunctionAnalysisManager &FAM) {
  // Gate on jeandle.java_method_compilation: only Java methods are analyzed
  // (template module / runtime stubs are skipped).
  Module *M = F.getParent();
  if (!M || !M->getNamedMetadata(jeandle::Metadata::JavaMethodCompilation))
    return jeandle::PEAResult();

  // Request DominatorTree and LoopInfo eagerly so they're cached for later
  // PEA passes that need them.
  auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
  auto &LI = FAM.getResult<LoopAnalysis>(F);

  Analyzer A(F, DT, LI);
  return A.run();
}
