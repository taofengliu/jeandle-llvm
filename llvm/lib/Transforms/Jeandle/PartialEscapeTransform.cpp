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

  // Step 5: collect the "deopt" operand bundle, if any, from the recorded
  // source (escape-point CallBase or original allocation).
  SmallVector<OperandBundleDef, 1> Bundles;
  if (E.DeoptBundleSource) {
    if (auto *CBSrc = dyn_cast<CallBase>(E.DeoptBundleSource)) {
      for (unsigned i = 0, n = CBSrc->getNumOperandBundles(); i < n; ++i) {
        auto BU = CBSrc->getOperandBundleAt(i);
        if (BU.getTagName() == "deopt") {
          SmallVector<Value *, 4> Inputs(BU.Inputs.begin(), BU.Inputs.end());
          Bundles.emplace_back("deopt", Inputs);
          break;
        }
      }
    }
  }

  // Step 6: emit the InvokeInst at the end of Origin.
  IRBuilder<> B(Origin);
  if (InsertBefore->getDebugLoc())
    B.SetCurrentDebugLocation(InsertBefore->getDebugLoc());
  InvokeInst *NewInv = B.CreateInvoke(AllocFn, /*NormalDest=*/MatCont,
                                      /*UnwindDest=*/UnwindDest,
                                      {Arg0, Arg1}, Bundles, "pea.mat");
  NewInv->setCallingConv(CallingConv::Hotspot_JIT);
  // Carry forward the precise return-type information so downstream lowering
  // can still see the exact klass.
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
  if (!OrigAlloc->use_empty())
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

  const auto &Result = FAM.getResult<PartialEscapeAnalysis>(F);
  if (!Result.hasOptimizationOpportunity())
    return PreservedAnalyses::all();

  bool Changed = false;

  ReversePostOrderTraversal<Function *> RPOT(&F);

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
        // The analyzer may have synthesized an unparented coercion
        // instruction (e.g. a `bitcast i32 ... to float`) as the replacement
        // when the stored Scalar's type doesn't match the load's type.
        // Splice it in immediately before Target so it dominates the RAUW'd
        // uses. Ownership transfers from PEAResult::OwnedInsts to the parent
        // BasicBlock; the OwnedInsts destructor skips inserted instructions.
        if (auto *RI = dyn_cast<Instruction>(Repl)) {
          if (RI->getParent() == nullptr) {
            RI->insertBefore(Target->getIterator());
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
        Changed = true;
        break;
      }
      case jeandle::PEAResult::EffectKind::ReplaceInput: {
        // Emitted by materializeAt for monitorenter/monitorexit call sites
        // whose elision was undone because the object escapes with locks
        // held. Replacement is OrigAlloc; resolve through NewAllocFor to
        // the live materialized invoke. (For non-Materialize cases,
        // E.Replacement is used verbatim.) The applyMaterialize RAUW would
        // also fix up the operand, but applying the explicit setOperand
        // here keeps the contract self-evident and guards against ordering
        // surprises.
        if (!E->Target || !E->Replacement)
          break;
        Value *Replacement = E->Replacement;
        if (auto *RI = dyn_cast<Instruction>(Replacement)) {
          auto It = NewAllocFor.find(RI);
          if (It != NewAllocFor.end())
            Replacement = It->second;
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

  return PreservedAnalyses::none();
}
