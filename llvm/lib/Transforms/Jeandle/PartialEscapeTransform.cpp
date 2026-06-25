//===- PartialEscapeTransform.cpp - PEA (transform pass) ------------------===//
//
// Copyright (c) 2026, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Consume the PEAResult from PartialEscapeAnalysis and apply effects in two
// ordered passes.
//
//   Pass 1 (non-CFG-kill effects): ReplaceLoad, ReplaceCall, ReplaceInput,
//   EliminateStore, Materialize, CreatePHI — applied per-block in RPO,
//   sorted by SeqNo.
//
//   Pass 2 (CFG-kill effects): EliminateAllocation — rewrites an invoke alloc
//   into an unconditional branch to the normal dest (dropping the unwind
//   edge), or plain-erases a call alloc.
//
// After both passes: ConstantFoldTerminator, a trivially-dead sweep, and
// EliminateUnreachableBlocks.
//
// At each escape point the Materialize handler emits a new Hotspot_JIT
// InvokeInst, replays tracked field stores, and RAUWs the original alloc's
// result onto the new pointer; Pass 2 then erases the now-dead original.
// The invoke reuses the original alloc's unwind dest when the original was
// itself an invoke, else synthesizes a minimal landingpad+resume block, and
// tags the return with java-klass, java-klass-exact, and nonnull.
//
// Lock cascade: when an object escapes with locks held, the analyzer drops
// the ReplaceCall(true) elisions of the unbalanced monitorenter sites and
// emits ReplaceInput effects pointing those calls' first operand at the
// materialized pointer (resolved via NewAllocFor / MatPerBlock). Matching
// downstream exits survive in IR with operands RAUW'd onto the new invoke.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Jeandle/PartialEscapeTransform.h"

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/Jeandle/PartialEscape.h"
#include "llvm/Analysis/Jeandle/PartialEscapeAnalysis.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Jeandle/Attributes.h"
#include "llvm/IR/Jeandle/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Local.h"

using namespace llvm;

static bool eraseAllocation(Instruction *Target) {
  assert(Target && "EliminateAllocation target must be non-null");

  if (auto *II = dyn_cast<InvokeInst>(Target)) {
    BasicBlock *Normal = II->getNormalDest();
    BasicBlock *Unwind = II->getUnwindDest();
    BasicBlock *Parent = II->getParent();

    // Null out any remaining uses before erasing.
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

// Emit the materialization sequence for a single Materialize effect: split the
// containing block at Effect.InsertBefore so the new materialization is the
// terminator, emit a hotspotcc InvokeInst, replay tracked field stores at the
// top of the normal-dest block, and RAUW the original allocation onto the new
// invoke. The same OrigAlloc may be materialized multiple times (mixed-state
// merge synthesizing a per-pred materialization on each virtual incoming):
// record each (analyzer-recorded-pred-block, OrigAlloc) → NewInv in
// MatPerBlock (CreatePHI picks the right per-incoming NewInv) and Origin →
// MatCont in BlockRename (so the PHI's incoming-block names the post-split
// merge-pred). Lock cascade: when the object had live locks, the analyzer's
// matching ReplaceInput effects run later in Pass 1 (after NewAllocFor /
// MatPerBlock are seeded here), and the RAUW below auto-updates monitor call
// operands.
//
// The materialized invoke is structurally identical to a frontend allocation
// site (hotspotcc `jeandle.new_instance` / `jeandle.newarray`, addrspace(1)
// return, exception edge), so the downstream GC-statepoint pipeline
// (PEA → InsertGCBarriers → ... → RewriteStatepointsForGC) wraps it
// uniformly with gc.statepoint/gc.result/gc.relocate; splitBasicBlock is
// SSA-preserving and the materialized pointer dominates every use in MatCont.
// See `partial-escape/310_full_pipeline_statepoint.ll`.
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

  // InsertBefore is a WeakTrackingVH. A lower-SeqNo ReplaceLoad/ReplaceCall on
  // the same instruction may have RAUW'd and erased it, nulling the handle;
  // dyn_cast_or_null<Instruction> covers that case, and we recover by
  // recomputing the safe IP at the head of the alloc's normal-dest block.
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
  // CallBase or original allocation). Drop "deopt": copying it would plant
  // OrigAlloc into NewInv's own bundle (the source CB's deopt slot for the VO
  // holds OrigAlloc), which the step-8 RAUW would rewrite to NewInv — a
  // self-reference the verifier rejects.
  // TODO(jeandle-deopt): see applyMaterialize().
  // Preserve every non-deopt bundle (funclet, gc-transition, cfguardtarget,
  // ptrauth, kcfi, ...). The funclet-bundle synthesis below handles the
  // Windows-EH case for the materialization site itself.
  SmallVector<OperandBundleDef, 4> Bundles;
  if (E.DeoptBundleSource) {
    if (auto *CBSrc = dyn_cast<CallBase>(E.DeoptBundleSource)) {
      SmallVector<OperandBundleDef, 4> All;
      CBSrc->getOperandBundlesAsDefs(All);
      for (OperandBundleDef &OBD : All)
        if (OBD.getTag() != "deopt")
          Bundles.emplace_back(std::move(OBD));
    }
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
  // Copy metadata and merge attrs from the original allocation so downstream
  // RewriteStatepointsForGC / GC barriers don't see weaker output (lost
  // !prof, !alias.scope, !noalias, !jeandle.bytecodeindex, nofree/nosync/cold).
  // Metadata first; addRetAttr below then takes precedence. Argument attrs are
  // safe to reuse because the invoke has the same {Arg0, Arg1} signature as a
  // frontend allocation site; return attrs are added explicitly below.
  if (OrigAlloc) {
    NewInv->copyMetadata(*OrigAlloc, /*WL=*/{});
    AttributeList OrigAttrs = OrigAlloc->getAttributes();
    AttributeList CurAttrs = NewInv->getAttributes();
    AttrBuilder RetAB(Ctx, CurAttrs.getRetAttrs());
    NewInv->setAttributes(OrigAttrs.addRetAttributes(Ctx, RetAB));
  }
  // Carry forward the precise return klass. Added after the merge so they
  // override the same Kind slot from the original.
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
  // becomes use-empty and is erased by Pass 2 (EliminateAllocation).
  //
  // IsPerPred: when multiple per-pred materializations of the same OrigAlloc
  // coexist, a global RAUW would substitute one pred's NewInv on every path,
  // which is SSA-invalid. Skip the global RAUW for per-pred materializes;
  // per-pred pre-merge uses are rewired via matching ReplaceInput effects
  // (MatPerBlock lookup) and post-merge uses via matching CreatePHI
  // (RAUWOrigToPHI). The original alloc still becomes use-empty by end of
  // Pass 1.
  //
  // TODO(jeandle-deopt): PEA is intentionally deopt-agnostic: it drops
  // operand bundles at materialization sites rather than maintaining a
  // materialized deopt bundle; a future deopt refactor should repopulate
  // them. Concretely, before the global RAUW we scrub OrigAlloc out of any
  // existing "deopt" operand bundle on a CallBase: letting the RAUW
  // propagate OrigAlloc -> NewInv into another sink CB's deopt bundle would
  // let a sibling Materialize for the same OrigAlloc copy that bundle and
  // hold NewInv from a non-dominating block. Replacing those operands with a
  // typed null keeps the bundle structurally intact for a future repopulate
  // while making the slot verifier-legal.
  if (!E.IsPerPred && !OrigAlloc->use_empty()) {
    Value *NullVO = ConstantPointerNull::get(
        cast<PointerType>(OrigAlloc->getType()));
    for (Use &U : llvm::make_early_inc_range(OrigAlloc->uses())) {
      auto *CB = dyn_cast<CallBase>(U.getUser());
      if (!CB)
        continue;
      unsigned OpIdx = U.getOperandNo();
      if (!CB->isBundleOperand(OpIdx))
        continue;
      if (CB->getOperandBundleForOperand(OpIdx).isDeoptOperandBundle())
        U.set(NullVO);
    }
  }
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
  // A per-pred Materialize replaces PH's terminator with a materialisation
  // invoke carrying an OOM unwind edge. If PH has multiple successors the OOM
  // would become observable on every PH→* edge — a Java-semantics change. So
  // for each IsPerPred Materialize on a PH with >1 successor, split the
  // PH→S edge where S is the merge that requested the per-pred mat (S has
  // >1 pred and BlockEffects[S] has a CreatePHI naming PH), then re-aim the
  // per-pred Materialize / ReplaceInput effects onto the new edge-block PH'
  // and seed BlockRename[PH] = PH' so CreatePHI's BlockRename-chain walk
  // routes the analyzer-recorded PH incoming through PH' to MatCont.
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
        // Skip the critical-edge split when PH carries any ReplaceInput effect
        // (un-elided monitorenter): that call site sits in PH and references
        // the materialised pointer via MatPerBlock[{PH, OrigAlloc}]. Moving
        // the Materialize to PH' would leave it without an SSA-dominating
        // receiver definition.
        // TODO(oom-on-split): the OOM-on-other-path issue remains open here
        // pending a richer un-elide model (per-pred monitorenter snapshot at
        // PH') or a LockState port.
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
        // When the replacement's stamp is wider than the original, a Pi
        // node would normally be injected. LLVM has no per-Value stamp at
        // this layer — the closest analogue is the load-only metadata
        // (`!nonnull`, `!dereferenceable`,
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
        // The analyzer may have synthesized an unparented coercion instruction
        // as the replacement (a same-bit-width `bitcast` reinterpretation).
        // Splice it, and any still-unparented operand, in postorder so each
        // operand is parented before its user; all land immediately before
        // Target. Ownership transfers from PEAResult::OwnedInsts to the parent
        // BasicBlock; OwnedInsts' destructor skips inserted instructions.
        //
        // TODO(unsafe-inliner): see PartialEscapeAnalysis.cpp (tier-2
        // dispatch). A PHINode replacement is owned by a CreatePHI effect that
        // runs LATER in SeqNo order. Splicing it here would parent it mid-block
        // (illegal for a PHINode) and crash CreatePHI's "must be unparented"
        // assert. Skip the splicing path for PHIs.
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
                  // A PHINode operand is owned by its own CreatePHI effect,
                  // which parents it at the merge-block head. Treat it as a
                  // leaf here: it is illegal mid-block, and parenting it now
                  // would crash the later CreatePHI handler. (A PHI dominates
                  // all non-PHI uses in its block, so it's always available.)
                  if (OpI->getParent() == nullptr && !isa<PHINode>(OpI) &&
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
        // JavaOp folded against a virtual receiver: non-void results are
        // replaced with a constant and the call erased; void JavaOps use a
        // null Replacement to request deletion only. Folded results that feed
        // `br i1` (monitorenter/exit, checkcast) leave constant terminators
        // cleaned up by ConstantFoldTerminator below.
        if (!E->Target)
          break;
        if (E->Replacement) {
          E->Target->replaceAllUsesWith(E->Replacement);
        } else if (!E->Target->use_empty()) {
          break;
        }
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
        // For the AllMaterialized + per-pred placeholder case, the matching
        // per-pred Materialize effects skipped their global RAUW, so
        // post-merge users of OrigAlloc still reference the original invoke.
        // RAUW them to the freshly built PHI here so downstream uses thread
        // through the per-pred PHI. Pre-merge per-pred uses were already
        // retargeted to per-pred NewInvs by the earlier ReplaceInput effects.
        //
        // GUARDED RAUW — a blanket replaceAllUsesWith would also rewrite PHI
        // users in blocks MergeBB does not dominate, producing a non-dominating
        // SSA use rejected by the verifier. The new PHI dominates MergeBB and
        // its dom-tree subtree but not MergeBB's predecessors. Strategy:
        //   * Non-PHI users: rewrite unconditionally (post-merge by
        //     construction — the analyzer only sets RAUWOrigToPHI for the
        //     AllMaterialized + per-pred placeholder case).
        //   * PHINode users: rewrite only the PEA-inserted Phi itself
        //     (self-incoming no-op skip). Other PHI users (e.g. a frontend
        //     Case-B alias PHI in MergeBB or a successor) are left alone;
        //     EliminateAllocation's poison RAUW dead-codes them.
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
        // Emitted for monitorenter/monitorexit sites whose elision was undone
        // because the object escapes with locks held. Replacement is OrigAlloc;
        // resolve to the live materialized invoke. Resolution priority:
        //   1. MatPerBlock[{E.Block, OrigAlloc}] — pred-specific NewInv. Needed
        //      when multiple per-pred materializations of the same OrigAlloc
        //      coexist (NewAllocFor only retains the last NewInv).
        //   2. NewAllocFor[OrigAlloc] — single-materialize fallback.
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
  for (BasicBlock &BB : llvm::make_early_inc_range(F)) {
    ConstantFoldTerminator(&BB, /*DeleteDeadConditions=*/true,
                           /*TLI=*/nullptr, /*DTU=*/nullptr);
  }

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

  // Drop references on still-unparented OwnedInsts before verifyFunction: the
  // PEAResult destructor (which runs in the analysis manager, after the
  // verifier) would clean them up, but without this sweep an unparented helper
  // holding a use of a now-parented helper trips the verifier's "use list of X
  // is in IR but X's user is not". dropAllReferences() severs the use list
  // without freeing the value; the dtor still owns the WeakTrackingVH and does
  // the eventual deleteValue.
  for (WeakTrackingVH &VH : Result.OwnedInsts) {
    if (Value *V = VH) {
      if (auto *I = dyn_cast<Instruction>(V)) {
        if (!I->getParent())
          I->dropAllReferences();
      }
    }
  }

  // In debug builds, verify the rewritten IR so any PEA malformation (broken
  // SSA from a stale RAUW, mis-ordered CreatePHI vs per-pred Materialize,
  // value-side virtual leak, critical-edge replacement, missing funclet
  // bundle, ...) is caught here with an actionable message rather than later
  // in RewriteStatepointsForGC or assembly emission.
#ifndef NDEBUG
  if (verifyFunction(F, &errs())) {
    errs() << "PEA: produced malformed IR for " << F.getName() << "\n";
    llvm_unreachable("PartialEscapeTransform produced malformed IR");
  }
#endif

  return PreservedAnalyses::none();
}
