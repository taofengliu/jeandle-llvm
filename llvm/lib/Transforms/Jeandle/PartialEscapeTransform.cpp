//===-- PartialEscapeTransform.cpp - PEA (transform pass) -----------------===//
//
// Part of the Jeandle JIT compiler.
//
// Consume the PEAResult produced by PartialEscapeAnalysis and apply effects
// in two ordered passes.
//
//   Pass 1 (non-CFG-kill effects): ReplaceLoad, ReplaceCall, ReplaceInput,
//   EliminateStore, Materialize, CreatePHI. These operate on individual
//   non-terminator instructions and are safe to apply per-block in RPO with
//   effects sorted by SeqNo.
//
//   Pass 2 (CFG-kill effects): EliminateAllocation. For invoke allocations
//   this rewrites the invoke into an unconditional branch to the normal dest
//   and drops the unwind edge; for call allocations it's a plain erase.
//
// After both passes we run ConstantFoldTerminator, a trivially-dead sweep,
// and EliminateUnreachableBlocks.
//
// The Materialize handler: at every escape point the analyzer marked, the
// transform emits a new allocation, replays the tracked field stores, and
// RAUWs the original allocation's result onto the new materialized pointer.
// The EliminateAllocation effect produced by Tier 1 then erases the
// now-dead original alloc.
//
// Materialization is emitted as an InvokeInst (CallingConv Hotspot_JIT):
//   - Splits the containing block at the escape point so the new invoke is a
//     terminator; the rest of the block becomes the normal-dest "mat.cont".
//   - Reuses the original allocation's unwind destination when the original
//     was itself an InvokeInst; otherwise synthesizes a minimal
//     landingpad+resume block.
//   - Preserves the "deopt" operand bundle from the escape-point CallBase if
//     present, falling back to the original allocation's bundle.
//   - Tags the return with java-klass, java-klass-exact, and nonnull.
//
// Lock cascade: when an object materializes with locks held, the analyzer
// drops the ReplaceCall(true) elisions of the unbalanced monitorenter call
// sites and emits ReplaceInput effects pointing the calls' first operand at
// the materialized pointer (resolved through NewAllocFor in the ReplaceInput
// handler). Matching downstream exits naturally survive in IR with their
// operand RAUW'd onto the new invoke.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Jeandle/PartialEscapeTransform.h"

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Jeandle/Attributes.h"
#include "llvm/IR/Jeandle/Metadata.h"
#include "llvm/IR/Jeandle/PartialEscape.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Jeandle/PartialEscapeAnalysis.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Local.h"

using namespace llvm;

static bool eraseAllocation(Instruction *Target) {
  assert(Target && "EliminateAllocation target must be non-null");

  if (auto *II = dyn_cast<InvokeInst>(Target)) {
    BasicBlock *Normal = II->getNormalDest();
    BasicBlock *Unwind = II->getUnwindDest();
    BasicBlock *Parent = II->getParent();

    // Defensive: if there are still uses (e.g., escape-cleanup races with
    // another effect path), null them out before erasing.
    if (!II->use_empty())
      II->replaceAllUsesWith(PoisonValue::get(II->getType()));

    // Remove the unwind edge: update any PHIs in the unwind block so they
    // forget the predecessor we're about to drop.
    Unwind->removePredecessor(Parent, /*KeepOneInputPHIs=*/true);

    // Replace the invoke with an unconditional branch to the normal dest.
    BranchInst::Create(Normal, Parent);
    II->eraseFromParent();
    return true;
  }

  if (auto *CI = dyn_cast<CallInst>(Target)) {
    if (!CI->use_empty())
      CI->replaceAllUsesWith(PoisonValue::get(CI->getType()));
    CI->eraseFromParent();
    return true;
  }

  return false;
}

// Synthesize a minimal landingpad+resume block at the end of F for use as
// the unwind destination of a materialization invoke. The frontend always
// emits the function's personality so this landingpad is well-formed.
static BasicBlock *createMinimalLandingpadBlock(Function &F) {
  LLVMContext &Ctx = F.getContext();
  BasicBlock *BB = BasicBlock::Create(Ctx, "pea.unwind", &F);
  IRBuilder<> B(BB);
  LandingPadInst *LP = B.CreateLandingPad(Type::getInt64Ty(Ctx), 0, "pea.lp");
  LP->setCleanup(true);
  B.CreateResume(LP);
  return BB;
}

// Pick (or synthesize) the unwind destination for a materialization invoke.
// Strategy 1: reuse the original allocation's unwind dest if the original
// was itself an InvokeInst (it's guaranteed landingpad-compatible because
// the frontend created it for OOM handling). Strategy 2 (fallback):
// synthesize a minimal landingpad+resume block.
static BasicBlock *findOrSynthesizeUnwindDest(Function &F,
                                              CallBase *OrigAlloc) {
  if (auto *OrigInv = dyn_cast<InvokeInst>(OrigAlloc)) {
    if (BasicBlock *UD = OrigInv->getUnwindDest())
      return UD;
  }
  return createMinimalLandingpadBlock(F);
}

// Emit the materialization sequence for a single Materialize effect.
// Splits the containing block at Effect.InsertBefore so the new materialization
// can be the terminator. Emits a hotspotcc InvokeInst (not CallInst) with the
// preserved "deopt" operand bundle from Effect.DeoptBundleSource, replays
// the tracked field stores at the top of the normal-dest block, and RAUWs
// the original allocation onto the new invoke.
//
// Lock cascade: when the object had live locks at materializeAt time, the
// analyzer has emitted ReplaceInput effects for the unbalanced enter call
// sites; those run later in Pass 1, after applyMaterialize records
// NewAllocFor[OrigAlloc] = NewInv. The RAUW below also auto-updates the
// monitor calls' operands as a side effect.
//
// The same OrigAlloc may be materialized multiple times (e.g. for a
// mixed-state merge that synthesizes a per-pred materialization on each
// virtual incoming). We record each (analyzer-recorded-pred-block, OrigAlloc)
// → NewInv in MatPerBlock so the CreatePHI handler can pick the right
// per-incoming NewInv. We also record block-rename Origin → MatCont in
// BlockRename so the PHI's incoming-block argument names the live merge-pred
// after splitBasicBlock.
//
// No virtual-anchor hook is emitted here. Graal's
// `virtualAnchorSupplier` (PartialEscapePhase.java:187) defaults to `null`
// for ALL hosted/Substrate tiers; it is overridden non-null only by
// `TruffleEarlyEscapeAnalysisPhase` to prevent VirtualObjects from floating
// into the merge of an exploded loop and being duplicated by Truffle's
// partial evaluator (TruffleEarlyEscapeAnchorNode). That is a Truffle-PE
// concern, not a Substrate-deopt or stack-map concern. Jeandle does not
// implement Truffle PE.
//
// For the GC-statepoint pipeline (PEA → InsertGCBarriers → ... →
// RewriteStatepointsForGC), the materialized invoke emitted below is
// structurally identical to a frontend allocation site: same hotspotcc
// invoke of `jeandle.new_instance` / `jeandle.newarray`, same addrspace(1)
// return, same optional "deopt" operand bundle, same exception edge. The
// follow-up addrspace(1) field stores in MatCont are tracked as GC roots
// from the moment the materialized base pointer is defined.
// RewriteStatepointsForGC wraps every such invoke uniformly with
// `gc.statepoint`+`gc.result` and rewrites uses through `gc.relocate`,
// preserving SSA/dominance across the block split (splitBasicBlock is
// SSA-preserving and the materialized pointer dominates every use in
// MatCont and its dominator-tree descendants). See lit test
// `partial-escape/310_full_pipeline_statepoint.ll` for end-to-end
// regression coverage (PEA + barriers + statepoint lowering on plain,
// mixed-merge, and per-pred PHI materializations).
static void applyMaterialize(Function &F, const jeandle::PEAResult &Result,
                             const jeandle::PEAResult::Effect &E,
                             DenseMap<Value *, Value *> &NewAllocFor,
                             DenseMap<std::pair<BasicBlock *, Value *>, Value *>
                                 &MatPerBlock,
                             DenseMap<BasicBlock *, BasicBlock *> &BlockRename) {
  assert(E.ObjID != jeandle::InvalidObjectID);
  assert(E.Target && "Materialize effect must carry the original allocation");

  jeandle::VirtualObject &VObj = *Result.VirtualObjects[E.ObjID];
  CallBase *OrigAlloc = VObj.AllocationCall;
  assert(OrigAlloc == E.Target);

  Module *M = F.getParent();
  LLVMContext &Ctx = M->getContext();

  // The InsertBefore handle is a WeakTrackingVH. If a lower-SeqNo
  // ReplaceLoad / ReplaceCall effect fired on the SafeIP instruction earlier
  // in this Pass 1 sweep, it RAUW'd the instruction (redirecting the handle to
  // a non-Instruction Value, e.g. a ConstantInt) and then erased it (nulling
  // the handle). dyn_cast_or_null<Instruction> covers both cases. When we
  // detect a dead IP, recover by recomputing the safe IP at the head of the
  // alloc's normal-dest block — semantically identical to what
  // computeMaterializationPoint would have returned afresh.
  Instruction *InsertBefore = dyn_cast_or_null<Instruction>(E.InsertBefore);
  if (!InsertBefore) {
    if (auto *II = dyn_cast<InvokeInst>(OrigAlloc))
      InsertBefore = &*II->getNormalDest()->getFirstNonPHIOrDbg();
    else
      InsertBefore = OrigAlloc->getNextNode();
  }
  assert(InsertBefore && "Materialize effect requires an insertion point");
  BasicBlock *Origin = InsertBefore->getParent();
  // Capture the analyzer's recorded pred BB at this effect (which may
  // already have been renamed by an earlier applyMaterialize at the same
  // pred) for the MatPerBlock key.
  BasicBlock *AnalyzerRecordedPred = E.Block;

  // Step 1: pick the allocation function.
  const char *FnName =
      VObj.isInstance() ? "jeandle.new_instance" : "jeandle.newarray";
  Function *AllocFn = M->getFunction(FnName);
  assert(AllocFn && "alloc function not declared in module");

  // Step 2: build args. Rebuild the klass pointer constant (rather than reuse
  // the original op) so we don't carry a stale operand reference.
  Value *Arg0 = ConstantExpr::getIntToPtr(
      ConstantInt::get(Type::getInt64Ty(Ctx),
                       static_cast<uint64_t>(VObj.Klass)),
      PointerType::get(Ctx, /*AS=*/0));
  Value *Arg1 = ConstantInt::get(
      Type::getInt32Ty(Ctx),
      VObj.isInstance() ? VObj.SizeInBytes : VObj.ArrayLength);

  // Step 3: find or synthesize the unwind destination.
  BasicBlock *UnwindDest = findOrSynthesizeUnwindDest(F, OrigAlloc);

  // Step 4: split the origin block at InsertBefore. SplitBlock leaves Origin
  // ending with an unconditional br to the new MatCont block; we drop that
  // terminator because the materialization invoke will take its place.
  BasicBlock *MatCont = Origin->splitBasicBlock(InsertBefore, "mat.cont");
  Origin->getTerminator()->eraseFromParent();

  // Step 5: collect operand bundles from the recorded source (escape-point
  // CallBase or original allocation). Preserve EVERY operand bundle (deopt,
  // funclet, gc-transition, cfguardtarget, ptrauth, kcfi, ...) — copying only
  // the first "deopt" bundle would silently drop all others. Additionally, if
  // the materialization-invoke insertion point is itself inside a Windows-EH
  // funclet, prepend a matching "funclet" bundle so the Verifier accepts the
  // call.
  SmallVector<OperandBundleDef, 4> Bundles;
  if (E.DeoptBundleSource) {
    if (auto *CBSrc = dyn_cast<CallBase>(E.DeoptBundleSource))
      CBSrc->getOperandBundlesAsDefs(Bundles);
  }
  // Synthesize a funclet bundle when the materialization site sits inside an
  // EH funclet pad and the recorded source didn't already supply one. Jeandle
  // is not currently on Windows, but the standing IR-defensiveness rule
  // requires PEA to tolerate any legal IR.
  bool HasFunclet = false;
  for (const OperandBundleDef &BD : Bundles)
    if (BD.getTag() == "funclet") {
      HasFunclet = true;
      break;
    }
  if (!HasFunclet) {
    if (auto *Pad = MatCont->getFirstNonPHI())
      if (auto *FPI = dyn_cast<FuncletPadInst>(Pad))
        Bundles.emplace_back("funclet", static_cast<Value *>(FPI));
  }

  // Step 6: emit the InvokeInst at the end of Origin.
  IRBuilder<> B(Origin);
  if (InsertBefore->getDebugLoc())
    B.SetCurrentDebugLocation(InsertBefore->getDebugLoc());
  InvokeInst *NewInv = B.CreateInvoke(AllocFn, /*NormalDest=*/MatCont,
                                      /*UnwindDest=*/UnwindDest,
                                      {Arg0, Arg1}, Bundles, "pea.mat");
  NewInv->setCallingConv(CallingConv::Hotspot_JIT);
  // Copy metadata and merge function/argument attributes from the original
  // allocation. Without this, downstream RewriteStatepointsForGC and
  // the GC barriers may produce strictly weaker output for re-materialised
  // objects (lost !prof, !alias.scope, !noalias, !jeandle.bytecode_index, and
  // function attrs like `nofree`, `nosync`, `cold`). Metadata is copied first
  // so the subsequent SetCurrentDebugLocation / addRetAttr calls take
  // precedence over anything the original carried.
  if (OrigAlloc) {
    NewInv->copyMetadata(*OrigAlloc, /*WL=*/{});
    // Merge original function-level + arg attrs while preserving the new
    // invoke's existing return attrs (the three set below are about to be
    // added explicitly — keep them authoritative on the new invoke).
    AttributeList OrigAttrs = OrigAlloc->getAttributes();
    AttributeList CurAttrs = NewInv->getAttributes();
    // Use the original's function + per-arg attrs as the base, then restore
    // the new invoke's return attrs (currently empty; the three Java-klass
    // ret attrs are appended below). Argument attrs from the original are
    // safe here because the materialization invoke uses the same {Arg0,
    // Arg1} positional signature as a frontend allocation site.
    AttrBuilder RetAB(Ctx, CurAttrs.getRetAttrs());
    NewInv->setAttributes(OrigAttrs.addRetAttributes(Ctx, RetAB));
  }
  // Carry forward the precise return-type information so downstream lowering
  // can still see the exact klass. Added AFTER attribute merge so they
  // override anything the original may have had at the same Kind slot.
  NewInv->addRetAttr(Attribute::get(Ctx, jeandle::Attribute::JavaKlass,
                                    std::to_string(VObj.Klass)));
  NewInv->addRetAttr(Attribute::get(Ctx, jeandle::Attribute::JavaKlassExact));
  NewInv->addRetAttr(Attribute::get(Ctx, Attribute::NonNull));

  // Step 7: replay tracked field stores at the top of MatCont.
  IRBuilder<> SB(MatCont, MatCont->getFirstInsertionPt());
  if (InsertBefore->getDebugLoc())
    SB.SetCurrentDebugLocation(InsertBefore->getDebugLoc());
  Type *I8 = Type::getInt8Ty(Ctx);
  for (const auto &FE : E.FieldEntries) {
    Value *V = nullptr;
    if (FE.Value.isScalar()) {
      V = FE.Value.getScalar();
    } else if (FE.Value.isMaterializedRef()) {
      V = FE.Value.getMaterialized();
      // Recursive nested-virtual materialization: if V is the original
      // allocation Instruction of an inner object whose Materialize effect
      // already ran (lower SeqNo), substitute the new materialized invoke so
      // we store the live materialized pointer rather than the soon-to-be-
      // erased OrigAlloc.
      if (auto *VI = dyn_cast<Instruction>(V)) {
        auto It = NewAllocFor.find(VI);
        if (It != NewAllocFor.end())
          V = It->second;
      }
    } else {
      // The analyzer rewrites every VirtualRef into MaterializedRef during
      // recursive prerequisite materialization. Unknown entries are filtered
      // out at snapshot time. Hitting either tag here is a contract violation.
      assert(false && "VirtualRef field entries must have been rewritten "
                      "to MaterializedRef during analysis");
      continue;
    }
    if (!V)
      continue;
    Value *Slot = SB.CreateInBoundsGEP(I8, NewInv, SB.getInt64(FE.Offset),
                                       "pea.matslot");
    StoreInst *S = SB.CreateAlignedStore(
        V, Slot, Align(V->getType()->isPointerTy() ? 8 : 1));
    // Java heap stores are atomic-unordered (matches jeandle-jdk emission).
    S->setAtomic(AtomicOrdering::Unordered);
  }

  // Record this materialization in NewAllocFor BEFORE the RAUW so any later
  // applyMaterialize can rewrite a recorded MaterializedRef referencing
  // OrigAlloc to the live NewInv.
  NewAllocFor[OrigAlloc] = NewInv;
  // Per-pred mapping for CreatePHI to pick the right NewInv per incoming.
  // Also record the block split so the PHI's incoming-block resolves to the
  // post-split MatCont.
  MatPerBlock[{AnalyzerRecordedPred, OrigAlloc}] = NewInv;
  BlockRename[Origin] = MatCont;

  // Step 8: RAUW the original allocation so every existing IR user of the
  // virtual pointer snaps to the new materialized pointer. The original alloc
  // is now use-empty and will be erased by Pass 2 (EliminateAllocation).
  //
  // IsPerPred: when multiple per-pred materializations of
  // the same OrigAlloc coexist, the FIRST applyMaterialize's global RAUW
  // would replace OrigAlloc's uses with THIS pred's NewInv — on other
  // preds' paths that's SSA-invalid. Skip the global RAUW for per-pred
  // materializes; per-pred pre-merge uses (un-elided enters) are rewired
  // via the matching ReplaceInput effects with MatPerBlock lookup, and
  // post-merge uses are rewired by the matching CreatePHI effect
  // (RAUWOrigToPHI). The original alloc still becomes use-empty by the
  // end of Pass 1.
  if (!E.IsPerPred && !OrigAlloc->use_empty())
    OrigAlloc->replaceAllUsesWith(NewInv);
}

static void collectSortedEffects(
    const SmallVectorImpl<jeandle::PEAResult::Effect> &Effects,
    SmallVectorImpl<const jeandle::PEAResult::Effect *> &Out) {
  Out.clear();
  Out.reserve(Effects.size());
  for (const auto &E : Effects)
    Out.push_back(&E);
  llvm::sort(Out, [](const jeandle::PEAResult::Effect *A,
                    const jeandle::PEAResult::Effect *B) {
    return A->SeqNo < B->SeqNo;
  });
}

PreservedAnalyses
PartialEscapeTransform::run(Function &F, FunctionAnalysisManager &FAM) {
  // Gate on jeandle.java_method_compilation.
  Module *M = F.getParent();
  if (!M || !M->getNamedMetadata(jeandle::Metadata::JavaMethodCompilation))
    return PreservedAnalyses::all();

  // Pre-Pass 1 may rewrite E.Block / E.InsertBefore for IsPerPred Materialize
  // effects sitting on critical-edge preds; take a non-const reference so we
  // can mutate the analysis result before applying it.
  auto &Result = FAM.getResult<PartialEscapeAnalysis>(F);
  if (!Result.hasOptimizationOpportunity())
    return PreservedAnalyses::all();

  bool Changed = false;

  // Map from each virtual object's original allocation to the new
  // materialized invoke produced by applyMaterialize. Populated in SeqNo
  // order across Pass 1 so that nested-virtual materializations record their
  // inner pointer before the outer's apply needs to look it up.
  DenseMap<Value *, Value *> NewAllocFor;
  // Per-pred (analyzer-recorded pred BB, OrigAlloc) → NewInv. Used by
  // CreatePHI to pick the right NewInv for each merge incoming when the same
  // OrigAlloc is materialized at multiple preds.
  DenseMap<std::pair<BasicBlock *, Value *>, Value *> MatPerBlock;
  // Block-split rename map: Origin BasicBlock → MatCont. Used to resolve
  // the analyzer-recorded PHI incoming block (which named the original pred
  // pre-split) to the live merge-pred (post-split MatCont chain).
  DenseMap<BasicBlock *, BasicBlock *> BlockRename;

  // -------------------------------------------------------------------------
  // PRE-PASS: split critical edges before per-pred materialisation.
  //
  // applyMaterialize replaces PH's terminator with the materialisation
  // invoke (whose unwind edge handles OOM). If PH has multiple successors,
  // the OOM is then observable on EVERY PH→* edge, not only on the
  // PH→merge edge that PEA intended — a Java-semantics change (paths that
  // originally did not allocate now can throw OutOfMemoryError).
  //
  // Fix: for every IsPerPred Materialize effect with Block = PH, look at
  // PH's successors. For each successor S that has multiple predecessors
  // AND for which we have a CreatePHI effect in BlockEffects[S] that
  // mentions PH in its PHIIncomingBlocks (i.e. S is the merge that
  // requested the per-pred materialisation), call SplitCriticalEdge on the
  // PH→S edge. Re-aim affected per-pred Materialize / ReplaceInput effects
  // from PH to the new edge-block PH', and seed BlockRename[PH] = PH' so
  // the CreatePHI handler's BlockRename-chain walk routes the analyzer-
  // recorded PH incoming through PH' to the post-split MatCont.
  {
    struct EdgeKey {
      BasicBlock *PH;
      unsigned SuccIdx;
    };
    SmallVector<EdgeKey, 4> Splits;
    DenseSet<std::pair<BasicBlock *, BasicBlock *>> SeenEdges;
    for (const auto &KvOut : Result.BlockEffects) {
      for (const auto &E : KvOut.second) {
        if (E.Kind != jeandle::PEAResult::EffectKind::Materialize ||
            !E.IsPerPred)
          continue;
        BasicBlock *PH = E.Block;
        if (!PH)
          continue;
        Instruction *Term = PH->getTerminator();
        if (!Term || Term->getNumSuccessors() <= 1)
          continue;
        // Skip the critical-edge split when PH carries any ReplaceInput
        // effect (un-elided monitorenter): the un-elided call site
        // sits in PH and references the materialised pointer through
        // MatPerBlock[{PH, OrigAlloc}]. Moving the Materialize to a new
        // edge-block PH' would leave the un-elided call in PH without an
        // SSA-dominating definition of the receiver. The OOM-on-other-
        // path issue still exists in this case; a future iteration will need
        // either a richer un-elide model (per-pred snapshot of the
        // monitorenter at PH' as well) or a different lock-encoding
        // strategy (a future LockState port). Documented limitation.
        bool HasReplaceInputInPH = false;
        for (const auto &E2 : KvOut.second) {
          if (E2.Kind == jeandle::PEAResult::EffectKind::ReplaceInput &&
              E2.Block == PH) {
            HasReplaceInputInPH = true;
            break;
          }
        }
        if (HasReplaceInputInPH)
          continue;
        for (unsigned i = 0, n = Term->getNumSuccessors(); i < n; ++i) {
          BasicBlock *S = Term->getSuccessor(i);
          if (S->hasNPredecessors(1))
            continue;
          // Check that BlockEffects[S] contains a CreatePHI that names PH
          // (i.e. S is the merge for which PEA emitted the per-pred mat).
          auto SIt = Result.BlockEffects.find(S);
          if (SIt == Result.BlockEffects.end())
            continue;
          bool MatchingPhi = false;
          for (const auto &PE : SIt->second) {
            if (PE.Kind != jeandle::PEAResult::EffectKind::CreatePHI)
              continue;
            for (BasicBlock *IB : PE.PHIIncomingBlocks) {
              if (IB == PH) {
                MatchingPhi = true;
                break;
              }
            }
            if (MatchingPhi)
              break;
          }
          if (!MatchingPhi)
            continue;
          auto Key = std::make_pair(PH, S);
          if (!SeenEdges.insert(Key).second)
            continue;
          Splits.push_back({PH, i});
        }
      }
    }
    DenseMap<BasicBlock *, BasicBlock *> PHRename;
    for (const EdgeKey &K : Splits) {
      Instruction *Term = K.PH->getTerminator();
      BasicBlock *NewBB = SplitCriticalEdge(
          Term, K.SuccIdx,
          CriticalEdgeSplittingOptions().setMergeIdenticalEdges());
      if (!NewBB)
        continue;
      NewBB->setName("pea.crit.split");
      PHRename[K.PH] = NewBB;
      // Seed the transform's BlockRename so the CreatePHI handler's chain
      // walk routes PH → NewBB and then (after applyMaterialize) NewBB →
      // MatCont.
      BlockRename[K.PH] = NewBB;
    }
    // Re-aim per-pred Materialize and PH-keyed ReplaceInput effects from
    // their original PH bucket onto the new edge-block. Also move the
    // BlockEffects entry so the RPO walk applies the effects at the
    // correct block boundary.
    if (!PHRename.empty()) {
      for (const auto &KvOut : PHRename) {
        BasicBlock *Old = KvOut.first;
        BasicBlock *New = KvOut.second;
        auto It = Result.BlockEffects.find(Old);
        if (It == Result.BlockEffects.end())
          continue;
        SmallVector<jeandle::PEAResult::Effect, 4> Stay;
        SmallVector<jeandle::PEAResult::Effect, 4> Move;
        for (auto &E : It->second) {
          bool MoveIt = false;
          if (E.Kind == jeandle::PEAResult::EffectKind::Materialize &&
              E.IsPerPred && E.Block == Old) {
            MoveIt = true;
          } else if (E.Kind == jeandle::PEAResult::EffectKind::ReplaceInput &&
                     E.Block == Old) {
            MoveIt = true;
          }
          if (MoveIt) {
            E.Block = New;
            if (E.Kind == jeandle::PEAResult::EffectKind::Materialize)
              E.InsertBefore = &*New->getFirstNonPHIOrDbg();
            Move.emplace_back(std::move(E));
          } else {
            Stay.emplace_back(std::move(E));
          }
        }
        It->second = std::move(Stay);
        for (auto &E : Move)
          Result.BlockEffects[New].emplace_back(std::move(E));
      }
    }
  }

  // Recompute RPOT AFTER any critical-edge splits so the new edge-blocks
  // are visited in Pass 1.
  ReversePostOrderTraversal<Function *> RPOT(&F);

  // -------------------------------------------------------------------------
  // Pass 1: non-CFG-kill effects (ReplaceLoad, ReplaceCall, ReplaceInput,
  // EliminateStore, Materialize, CreatePHI)
  // -------------------------------------------------------------------------
  for (BasicBlock *BB : RPOT) {
    auto It = Result.BlockEffects.find(BB);
    if (It == Result.BlockEffects.end())
      continue;

    SmallVector<const jeandle::PEAResult::Effect *, 8> Sorted;
    collectSortedEffects(It->second, Sorted);

    for (const auto *E : Sorted) {
      switch (E->Kind) {
      case jeandle::PEAResult::EffectKind::ReplaceLoad: {
        if (!E->Target || !E->Replacement)
          break;
        Instruction *Target = E->Target;
        Value *Repl = E->Replacement;
        // Graal's GraphUtil.replaceAtUsages injects a Pi
        // node when the replacement's stamp is wider than the original.
        // LLVM has no per-Value stamp at this layer — the closest analogue
        // is the load-only metadata (`!nonnull`, `!dereferenceable`,
        // `!dereferenceable_or_null`, `!align`, `!invariant.load`,
        // `!noundef`) the original load may have carried. Transfer those
        // (only when both sides are LoadInsts and the Replacement is
        // missing the kind) so downstream LLVM passes do not lose the
        // narrower-than-default knowledge after RAUW.
        //
        // Skipped intentionally:
        //  * `!range`: not monotonic — a value that's range-narrowed via
        //    the original load is not guaranteed to satisfy the same range
        //    when sourced from a freshly stored Scalar. Conservative skip.
        //  * Non-load Replacement (Constant, binop, bitcast, struct):
        //    load-only metadata does not apply.
        //  * Non-load Target (atomicrmw / cmpxchg / icmp folded via
        //    ReplaceLoad's generic RAUW handler): no load metadata on the
        //    source.
        if (auto *TargetLoad = dyn_cast<LoadInst>(Target)) {
          if (auto *ReplLoad = dyn_cast<LoadInst>(Repl)) {
            static constexpr unsigned PreservableKinds[] = {
                LLVMContext::MD_nonnull,
                LLVMContext::MD_dereferenceable,
                LLVMContext::MD_dereferenceable_or_null,
                LLVMContext::MD_align,
                LLVMContext::MD_invariant_load,
                LLVMContext::MD_noundef,
            };
            for (unsigned K : PreservableKinds) {
              if (ReplLoad->getMetadata(K))
                continue; // already at least as precise; do not overwrite.
              if (MDNode *MD = TargetLoad->getMetadata(K))
                ReplLoad->setMetadata(K, MD);
            }
          }
        }
        // The analyzer may have synthesized one or more unparented coercion
        // instructions as the replacement: a `bitcast` for same-bit-width
        // primitive↔primitive, or a `lshr`+`trunc` chain for narrowing
        // primitive bit-widths on integer loads. Splice the chain in postorder
        // so each operand is parented before its user; all land immediately
        // before Target. Ownership transfers from PEAResult::OwnedInsts to the
        // parent BasicBlock; the OwnedInsts destructor skips inserted
        // instructions.
        //
        // A PHINode replacement is owned by a CreatePHI effect that
        // runs LATER in SeqNo order (drain-time reassignment). Splicing the
        // PHI before Target here would (a) parent it at the wrong location
        // (mid-block instead of merge-block head, which is illegal for a
        // PHINode) and (b) crash the CreatePHI handler's "must be unparented"
        // assert. Skip the splicing path for PHIs; the matching CreatePHI
        // will insert at the correct location.
        if (auto *RI = dyn_cast<Instruction>(Repl); RI && !isa<PHINode>(RI)) {
          SmallVector<Instruction *, 4> Stack;
          SmallPtrSet<Instruction *, 4> Visited;
          if (RI->getParent() == nullptr && Visited.insert(RI).second)
            Stack.push_back(RI);
          // Iterative postorder splice: mark on first visit, splice on second.
          SmallVector<Instruction *, 4> PostOrder;
          // Use a small worklist that tracks (node, visited-children).
          struct Frame {
            Instruction *I;
            unsigned NextOpIdx;
          };
          SmallVector<Frame, 4> Frames;
          if (!Stack.empty()) {
            Frames.push_back({Stack.back(), 0});
            while (!Frames.empty()) {
              Frame &Top = Frames.back();
              if (Top.NextOpIdx < Top.I->getNumOperands()) {
                Value *Op = Top.I->getOperand(Top.NextOpIdx++);
                if (auto *OpI = dyn_cast<Instruction>(Op)) {
                  if (OpI->getParent() == nullptr &&
                      Visited.insert(OpI).second) {
                    Frames.push_back({OpI, 0});
                  }
                }
              } else {
                PostOrder.push_back(Top.I);
                Frames.pop_back();
              }
            }
          }
          for (Instruction *I : PostOrder) {
            if (I->getParent() == nullptr)
              I->insertBefore(Target->getIterator());
          }
          // Defensive: if the coercion's operand happens to be an OrigAlloc
          // that's been materialized, redirect through NewAllocFor. Coercion
          // operands are stored Scalars (typed primitives), so this branch
          // is normally inert; mirrors the pattern in other handlers.
          auto It2 = NewAllocFor.find(RI);
          if (It2 != NewAllocFor.end())
            Repl = It2->second;
        }
        if (!Target->use_empty())
          Target->replaceAllUsesWith(Repl);
        Target->eraseFromParent();
        Changed = true;
        break;
      }
      case jeandle::PEAResult::EffectKind::ReplaceCall: {
        // JavaOp folded against a virtual receiver. The call site's result
        // is replaced with a constant and the call is erased. Some folded
        // JavaOps (monitorenter/exit, checkcast) feed `br i1` on their
        // result; the constant-folded terminator is cleaned up below by
        // ConstantFoldTerminator after both Pass 1 and Pass 2 finish.
        if (!E->Target || !E->Replacement)
          break;
        E->Target->replaceAllUsesWith(E->Replacement);
        // For InvokeInst we cannot simply erase — the unwind edge must be
        // dropped first. JavaOp folds emit `call` (not invoke) calls in
        // practice, so handle that common case; defensively skip invokes
        // (no JavaOp test currently uses invoke for a fold target).
        if (auto *II = dyn_cast<InvokeInst>(E->Target)) {
          BasicBlock *Normal = II->getNormalDest();
          BasicBlock *Unwind = II->getUnwindDest();
          BasicBlock *Parent = II->getParent();
          Unwind->removePredecessor(Parent, /*KeepOneInputPHIs=*/true);
          BranchInst::Create(Normal, Parent);
          II->eraseFromParent();
        } else {
          E->Target->eraseFromParent();
        }
        Changed = true;
        break;
      }
      case jeandle::PEAResult::EffectKind::EliminateStore: {
        if (!E->Target)
          break;
        E->Target->eraseFromParent();
        Changed = true;
        break;
      }
      case jeandle::PEAResult::EffectKind::Materialize: {
        // Emit the materialization sequence and RAUW the original allocation
        // onto the new CallInst. The original allocation itself is erased
        // later by EliminateAllocation in Pass 2.
        applyMaterialize(F, Result, *E, NewAllocFor, MatPerBlock, BlockRename);
        Changed = true;
        break;
      }
      case jeandle::PEAResult::EffectKind::CreatePHI: {
        // Insert the unparented PHI created by the analyzer at the head
        // of the merge block (after any existing PHIs), and wire up its
        // incoming values. For each incoming (V, Pred):
        //   * Walk BlockRename forward to the live merge-pred (post-split
        //     MatCont chain).
        //   * If V refers to an OrigAlloc that's been materialized at this
        //     specific (Pred, V), use the per-pred NewInv from MatPerBlock.
        //     Otherwise fall back to NewAllocFor for the single-mat path.
        PHINode *Phi = E->PhiInst;
        assert(Phi && "CreatePHI effect requires a PhiInst");
        assert(Phi->getParent() == nullptr &&
               "CreatePHI's PhiInst must be unparented at apply time");
        BasicBlock *MergeBB = E->Block;
        Phi->insertBefore(MergeBB->getFirstInsertionPt());
        assert(E->PHIIncomingValues.size() == E->PHIIncomingBlocks.size());
        for (unsigned I = 0; I < E->PHIIncomingValues.size(); ++I) {
          Value *V = E->PHIIncomingValues[I];
          BasicBlock *Pred = E->PHIIncomingBlocks[I];
          if (auto *VI = dyn_cast<Instruction>(V)) {
            auto It = MatPerBlock.find({Pred, VI});
            if (It != MatPerBlock.end()) {
              V = It->second;
            } else {
              auto It2 = NewAllocFor.find(VI);
              if (It2 != NewAllocFor.end())
                V = It2->second;
            }
          }
          // Resolve the live pred BB through BlockRename.
          BasicBlock *LivePred = Pred;
          while (true) {
            auto It = BlockRename.find(LivePred);
            if (It == BlockRename.end())
              break;
            LivePred = It->second;
          }
          Phi->addIncoming(V, LivePred);
        }
        // For the AllMaterialized + per-pred placeholder
        // case, the matching per-pred Materialize effects skipped their
        // global RAUW. Post-merge users of OrigAlloc still reference the
        // original allocation invoke. RAUW them here to the freshly built
        // PHI so downstream loads/calls thread through the per-pred PHI
        // rather than referencing one pred's NewInv on the other pred's
        // path. Pre-merge per-pred uses (e.g., un-elided enter calls in
        // PH or its MatCont chain) were already retargeted to per-pred
        // NewInvs by the ReplaceInput effects (which run earlier in
        // SeqNo order, within BlockEffects[PH]); their operand-sets
        // happen BEFORE this CreatePHI runs, so the RAUW only affects
        // uses that still reference OrigAlloc — i.e., post-merge users.
        // GUARDED RAUW. A blanket replaceAllUsesWith would substitute
        // every remaining use of OrigAlloc with the new PHI, including PHI
        // users in blocks that the new PHI's parent (MergeBB) does NOT
        // dominate. The new PHI dominates MergeBB and its dom-tree subtree
        // but not the predecessors of MergeBB, so any pre-existing PHI on a
        // pred-edge that named OrigAlloc would get retargeted to a value
        // that doesn't dominate its use — an SSA violation rejected by
        // Verifier (verifyFunction will assert in debug builds).
        //
        // Strategy:
        //   * Non-PHI users: rewrite unconditionally — they were dominated
        //     by OrigAlloc and OrigAlloc was dominated by MergeBB
        //     (post-merge by construction; the analyzer only sets
        //     RAUWOrigToPHI for the AllMaterialized + per-pred placeholder
        //     case where uses are post-merge).
        //   * PHINode users: rewrite ONLY if PN is the PEA-inserted Phi
        //     itself (a self-incoming no-op skip). Other PHI users — most
        //     notably any pre-existing frontend-emitted PHI in MergeBB or a
        //     successor that names OrigAlloc — are left alone. They will
        //     either be dead-coded by EliminateAllocation's RAUW to
        //     PoisonValue (Case-B PHIs that the front-end emitted as a
        //     no-op duplicate of our Case-A PHI) or be resolved later by
        //     the eventual full deletion.
        if (E->RAUWOrigToPHI && E->ObjID != jeandle::InvalidObjectID) {
          jeandle::VirtualObject &VObj = *Result.VirtualObjects[E->ObjID];
          if (VObj.AllocationCall && !VObj.AllocationCall->use_empty()) {
            SmallVector<Use *, 8> WorkList;
            for (Use &U : VObj.AllocationCall->uses())
              WorkList.push_back(&U);
            for (Use *U : WorkList) {
              User *Usr = U->getUser();
              if (auto *PN = dyn_cast<PHINode>(Usr)) {
                if (PN == Phi)
                  continue; // our own PHI's incomings were set above.
                if (PN->getParent() != Phi->getParent())
                  continue; // SSA-unsafe across non-dominating blocks.
              }
              U->set(Phi);
            }
          }
        }
        Changed = true;
        break;
      }
      case jeandle::PEAResult::EffectKind::ReplaceInput: {
        // Emitted by materializeAt / materializeAtPredFromExitInfo for
        // monitorenter/monitorexit call sites whose elision was undone
        // because the object escapes with locks held. Replacement is
        // OrigAlloc; resolve to the live materialized invoke.
        //
        // Resolution priority:
        //   1. MatPerBlock[{E.Block, OrigAlloc}] — when E.Block carries the
        //      per-pred BB (set by materializeAtPredFromExitInfo for the
        //      lock-mismatch / cascade-into-mixed paths), this returns the
        //      pred-specific NewInv. Critical when multiple per-pred
        //      materializations of the same OrigAlloc coexist; the global
        //      NewAllocFor only retains the last-written NewInv and would
        //      yield SSA-broken cross-pred operands otherwise.
        //   2. NewAllocFor[OrigAlloc] — single-materialize fallback for the
        //      classic in-block escape path (materializeAt). Mirrors the
        //      RAUW that applyMaterialize performed.
        if (!E->Target || !E->Replacement)
          break;
        Value *Replacement = E->Replacement;
        if (auto *RI = dyn_cast<Instruction>(Replacement)) {
          if (E->Block) {
            auto It = MatPerBlock.find({E->Block, RI});
            if (It != MatPerBlock.end()) {
              Replacement = It->second;
            } else {
              auto It2 = NewAllocFor.find(RI);
              if (It2 != NewAllocFor.end())
                Replacement = It2->second;
            }
          } else {
            auto It2 = NewAllocFor.find(RI);
            if (It2 != NewAllocFor.end())
              Replacement = It2->second;
          }
        }
        E->Target->setOperand(E->InputIndex, Replacement);
        Changed = true;
        break;
      }
      default:
        // EliminateAllocation is handled in Pass 2.
        break;
      }
    }
  }

  // -------------------------------------------------------------------------
  // Pass 2: CFG-kill effects (EliminateAllocation).
  // -------------------------------------------------------------------------
  for (BasicBlock *BB : RPOT) {
    auto It = Result.BlockEffects.find(BB);
    if (It == Result.BlockEffects.end())
      continue;

    SmallVector<const jeandle::PEAResult::Effect *, 8> Sorted;
    collectSortedEffects(It->second, Sorted);

    for (const auto *E : Sorted) {
      if (E->Kind == jeandle::PEAResult::EffectKind::EliminateAllocation)
        Changed |= eraseAllocation(E->Target);
    }
  }

  // Erase parented Case-B alias PHIs that the analyzer flagged
  // as redundant for NeverEscapes VOs. Pass 2 above already RAUW'd
  // every OrigAlloc incoming to poison via eraseAllocation, so the
  // PHIs survive as `phi [poison, poison]`. Replace each with poison
  // and erase. WeakTrackingVH auto-nulls if some other code path
  // already deleted the PHI (e.g. an outer iteration's dead-block
  // sweep), so the null check below is load-bearing.
  for (auto &VH : Result.CaseBAliasedPhisToErase) {
    Value *V = VH;
    if (!V)
      continue;
    auto *Phi = dyn_cast<PHINode>(V);
    if (!Phi || !Phi->getParent())
      continue;
    Phi->replaceAllUsesWith(PoisonValue::get(Phi->getType()));
    Phi->eraseFromParent();
    Changed = true;
  }

  if (!Changed)
    return PreservedAnalyses::all();

  // Folded JavaOps may have left behind `br i1 true|false, ...` terminators
  // (monitorenter elision RAUWs the result to `true`, after which a
  // conditional branch on the result becomes constant). Use
  // ConstantFoldTerminator to collapse those before the unreachable-block
  // sweep so the slow-path blocks of synchronized regions get cleaned up.
  bool TermFolded = false;
  for (BasicBlock &BB : llvm::make_early_inc_range(F)) {
    if (ConstantFoldTerminator(&BB, /*DeleteDeadConditions=*/true,
                               /*TLI=*/nullptr, /*DTU=*/nullptr))
      TermFolded = true;
  }
  (void)TermFolded;

  // Sweep trivially-dead instructions that became unused after our rewrites
  // (e.g., GEPs derived from eliminated allocations whose only users were the
  // loads/stores we replaced in Pass 1). Iterate to fixpoint so cascading
  // deaths are caught.
  bool LocalChanged = true;
  while (LocalChanged) {
    LocalChanged = false;
    for (BasicBlock &BB : llvm::make_early_inc_range(F)) {
      for (Instruction &I : llvm::make_early_inc_range(BB)) {
        if (isInstructionTriviallyDead(&I)) {
          I.eraseFromParent();
          LocalChanged = true;
        }
      }
    }
  }

  // Clean up unwind blocks that became unreachable after invoke→br rewrites
  // and slow-path blocks orphaned by ConstantFoldTerminator above.
  EliminateUnreachableBlocks(F);

  // Drop references on still-unparented OwnedInsts so the verifier does
  // not flag them via "use list of <parented inst> contains <unparented>".
  // The PEAResult destructor would clean these up, but the destructor runs
  // AFTER the verifier (PEAResult lives in the analysis manager). Without
  // this sweep, an unparented helper Instruction (e.g. an unparented helper
  // like `pea.cas.sel` when no downstream load consumed the post-CAS slot
  // value) holds a use of a now-parented helper (e.g. `pea.cas.eq`), which
  // the LLVM verifier reports as "use list of X is in IR but X's user is
  // not".
  //
  // Clearing operands via dropAllReferences() severs the use list without
  // freeing the value; the PEAResult dtor still owns the WeakTrackingVH and
  // does the eventual deleteValue.
  for (WeakTrackingVH &VH : Result.OwnedInsts) {
    if (Value *V = VH) {
      if (auto *I = dyn_cast<Instruction>(V)) {
        if (!I->getParent())
          I->dropAllReferences();
      }
    }
  }

  // In debug builds, run llvm::verifyFunction on the rewritten IR so
  // any malformation produced by a PEA effect (broken SSA from a stale RAUW,
  // mis-ordered CreatePHI vs per-pred Materialize, value-side virtual leak,
  // critical-edge replacement, missing operand bundles in funclets, ...) is
  // caught at the iteration boundary with an actionable assertion message
  // rather than exploding much later in RewriteStatepointsForGC or assembly
  // emission.
#ifndef NDEBUG
  if (verifyFunction(F, &errs())) {
    errs() << "PEA: produced malformed IR for " << F.getName() << "\n";
    llvm_unreachable("PartialEscapeTransform produced malformed IR");
  }
#endif

  return PreservedAnalyses::none();
}
