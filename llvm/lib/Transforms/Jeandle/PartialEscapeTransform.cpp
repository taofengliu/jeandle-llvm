//===- PartialEscapeTransform.cpp - PEA (transform pass) ------------------===//
//
// Copyright (c) 2026, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Consume the PEAResult from PartialEscapeAnalysis and apply its effects in
// two ordered passes driven by Effect::isCfgKill() (Jeandle's analog of
// Graal's EffectList.apply(graph, obsoleteNodes, cfgKills)).
//
//   Pass 1 (non-cfgKill): ReplaceLoad, ReplaceCall, EliminateStore,
//   Materialize, CreatePHI, RewriteDeoptBundle, RewritePhiIncoming — applied
//   per-block in RPO via EffectList::apply, which sorts by SeqNo and
//   dispatches each effect's apply() through TransformContext.
//
//   Pass 2 (cfgKill): EliminateAllocation — rewrites a NeverEscapes invoke
//   alloc into an unconditional branch (dropping the unwind edge) or plain-
//   erases a call alloc. isCfgKill() is true ONLY for this effect.
//
// Materialization model: a PartiallyEscapes VO materializes by replaying its
// tracked field stores and re-emitting its surviving monitorenters onto its
// ORIGINAL allocation (OrigAlloc = VObj.AllocationCall), which dominates every
// escape point and is kept alive (EliminateAllocation is suppressed for
// PartiallyEscapes). OrigAlloc already carries the correct allocation-site
// deopt operand bundle. NeverEscapes VOs are eliminated (OrigAlloc erased) and
// described by a deopt-bundle descriptor (HotSpot reallocs at deopt). OrigAlloc
// is cast to CallBase, not InvokeInst: a self-loop-header alloc uses the call
// form (an invoke's normal dest is always a distinct block). The InsertBefore
// eager-update hook (relocateDependentMaterializes) is retained because a
// sibling fold can still erase E.InsertBefore.
//
// When several objects escape at one escape point, their interleaved locks on
// the runtime lock stack MUST be re-emitted as ONE globally depth-sorted list
// (computeEscapePointLocks), emitted once by the highest-SeqNo materialize at
// that point, each receiver resolved to the sibling's OrigAlloc via NewInvOf.
// Per-object lock emission would mis-order re-entrant interleaved lock stacks.
//
// After both passes: ConstantFoldTerminator, a trivial-PHI fold, a dead-code
// sweep, and EliminateUnreachableBlocks.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Jeandle/PartialEscapeTransform.h"

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/Jeandle/PartialEscape.h"
#include "llvm/Analysis/Jeandle/PartialEscapeAnalysis.h"
#include "llvm/Analysis/Jeandle/PartialEscapeUtils.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/EHPersonalities.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Jeandle/Attributes.h"
#include "llvm/IR/Jeandle/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Jeandle/JeandleTransformUtils.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Local.h"

using namespace llvm;

static bool eraseAllocation(Instruction *Target) {
  assert(Target && "EliminateAllocation target must be non-null");

  if (auto *II = dyn_cast<InvokeInst>(Target)) {
    // The transform is the last line of defense: dropping the unwind edge is
    // only sound because the analyzer proved this allocation never observably
    // escapes, and a Jeandle allocation intrinsic's exception edge handles
    // OOM only (unobservable — re-thrown identically by the materialized
    // invoke or, for NeverEscapes, never taken). Assert the analyzer's
    // NeverEscapes contract so a future misclassification of a side-effecting
    // invoke as eliminable fails loudly instead of silently miscompiling.
    assert(jeandle::pea::isJeandleAllocation(II) &&
           "EliminateAllocation may only drop the unwind edge of a Jeandle "
           "allocation intrinsic (OOM-only, unobservable throw)");
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
    // Same NeverEscapes contract as the invoke branch above (a call-form
    // allocation has no unwind edge to drop, but the eliminability guarantee
    // is identical).
    assert(jeandle::pea::isJeandleAllocation(CI) &&
           "EliminateAllocation target must be a Jeandle allocation intrinsic");
    if (!CI->use_empty())
      CI->replaceAllUsesWith(PoisonValue::get(CI->getType()));
    CI->eraseFromParent();
    return true;
  }

  return false;
}

// Emit the materialization sequence for a single Materialize effect: replay
// tracked field stores and re-emit surviving monitorenters onto OrigAlloc
// immediately before the escape point (see file header for the
// materialization model). The downstream GC-statepoint pipeline
// (PEA → InsertGCBarriers → ... → RewriteStatepointsForGC) wraps the original
// allocation invoke with gc.statepoint/gc.result/gc.relocate; the replayed
// stores land before the escape point and OrigAlloc dominates them.
// See `partial-escape/310_full_pipeline_statepoint.ll`.

// Eager-update hook: call this BEFORE erasing `Dying` from IR. Re-aims every
// Materialize whose InsertBefore == Dying to `Next` (the in-block normal-flow
// successor — for a non-terminator Target->getNextNode(); for an invoke
// terminator, the `br` created by BranchInst::Create, captured as
// II->getNextNode() after Create but before erase). This keeps the
// WeakTrackingVH alive so applyMaterialize never sees a null InsertBefore.
// Still required because a Materialize's InsertBefore can be a folded JavaOp
// invoke terminator that a sibling ReplaceCall erases in the same block bucket
// (lower SeqNo). Re-aiming to the successor (same program point, same block)
// is sound: every replayed field value that dominated the erased instruction
// also dominates its in-block successor. Mirrors Graal's "fixed deleted ->
// use node.next()" pattern (MATERIALIZE_ALL).
// Re-indexes each dependent into Next's bucket so a future erase of Next
// chains correctly.
static void relocateDependentMaterializes(
    DenseMap<Instruction *, SmallVector<jeandle::MaterializeEffect *, 4>>
        &Dependents,
    Instruction *Dying, Instruction *Next) {
  if (!Next || Next == Dying)
    return;
  auto It = Dependents.find(Dying);
  if (It == Dependents.end())
    return;
  // Move the bucket out and erase Dying's entry BEFORE any insert: inserting
  // into Dependents[Next] below could rehash and invalidate `It`.
  SmallVector<jeandle::MaterializeEffect *, 4> Bucket = std::move(It->second);
  Dependents.erase(It);
  auto &NextBucket = Dependents[Next];
  for (jeandle::MaterializeEffect *M : Bucket) {
    M->setInsertBefore(Next);
    NextBucket.push_back(M);
  }
}

static void applyMaterialize(
    Function &F, const jeandle::PEAResult &Result,
    const jeandle::MaterializeEffect &E,
    DenseMap<const jeandle::MaterializeEffect *, CallBase *> &NewInvOf,
    const DenseMap<const jeandle::MaterializeEffect *, Instruction *>
        &OrigInsertBefore) {
  assert(E.ObjID != jeandle::InvalidObjectID);
  assert(E.Target && "Materialize effect must carry the original allocation");

  jeandle::VirtualObject &VObj = *Result.VirtualObjects[E.ObjID];
  CallBase *OrigAlloc = VObj.AllocationCall;
  assert(OrigAlloc == E.Target);

  Module *M = F.getParent();
  LLVMContext &Ctx = M->getContext();
  const DataLayout &DL = M->getDataLayout();

  // The materialization value is OrigAlloc (VObj.AllocationCall); see the file
  // header for the model. applyMaterialize is reached only for PartiallyEscapes
  // VOs (NeverEscapes go to EliminateAllocation; AlwaysEscapes effects were
  // dropped by the analyzer). OrigAlloc is a CallBase: typically an InvokeInst
  // (a Jeandle allocation intrinsic carries an OOM unwind edge), but the
  // frontend emits a call-form allocation when the alloc must live inside its
  // own self-loop header (an invoke's normal dest is always a distinct block)
  // — so cast to CallBase, not InvokeInst.
  CallBase *MatVal = cast<CallBase>(OrigAlloc);
  NewInvOf[&E] = MatVal;

  Instruction *InsertBefore = dyn_cast_or_null<Instruction>(E.InsertBefore);
  assert(InsertBefore &&
         "Materialize InsertBefore was null at apply time — a sibling erase "
         "should have re-aimed it via relocateDependentMaterializes");
  BasicBlock *Origin = InsertBefore->getParent();

  // Replay field stores and re-emit locks immediately before the escape point.
  IRBuilder<> SB(Origin, InsertBefore->getIterator());
  if (InsertBefore->getDebugLoc())
    SB.SetCurrentDebugLocation(InsertBefore->getDebugLoc());
  Type *I8 = Type::getInt8Ty(Ctx);

  // Replay this object's tracked field stores onto OrigAlloc, immediately
  // before the escape point, so the object's fields hold their current values
  // when it escapes. OrigAlloc and every field value dominate this point
  // (analyzer per-field dominance invariant). A nested/peer virtual's field
  // value is its own OrigAlloc (the analyzer rewrites
  // VirtualRef→MaterializedRef during prerequisite materialization), which also
  // dominates here.
  for (const auto &FE : E.FieldEntries) {
    Value *V = nullptr;
    if (FE.Value.isScalar()) {
      V = FE.Value.getScalar();
    } else if (FE.Value.isMaterializedRef()) {
      V = FE.Value.getMaterialized();
    } else {
      // The analyzer rewrites every VirtualRef into MaterializedRef during
      // recursive prerequisite materialization; unknown entries are filtered at
      // snapshot time. Hitting another tag here is a contract violation.
      assert(false && "VirtualRef field entries must have been rewritten to "
                      "MaterializedRef during analysis");
      continue;
    }
    if (!V)
      continue;
    Value *Slot =
        SB.CreateInBoundsGEP(I8, MatVal, SB.getInt64(FE.Offset), "pea.matslot");
    // Natural alignment = the field type's store size rounded up to a power of
    // two (atomic-unordered stores MUST be naturally aligned; ABI align may be
    // smaller than store size, e.g. i64 under the default datalayout). Derived
    // from the DataLayout so it stays correct under a future compressed-oop /
    // 32-bit heap model, and matches the frontend's natural-aligned emission.
    uint64_t StoreSz = DL.getTypeStoreSize(V->getType()).getFixedValue();
    Align NaturalAlign(llvm::PowerOf2Ceil(StoreSz ? StoreSz : 1));
    StoreInst *S = SB.CreateAlignedStore(V, Slot, NaturalAlign);
    S->setAtomic(AtomicOrdering::Unordered); // Java heap stores are unordered
  }

  // Re-emit surviving monitorenters onto OrigAlloc. Locks from MULTIPLE objects
  // escaping at the same point are interleaved on the runtime lock stack, so
  // they must be re-emitted as ONE globally depth-sorted list; the analyzer
  // merged them per escape point into Result.EscapePointLocks. The highest-SeqNo
  // effect at this escape point emits the merged list once — by then every
  // sibling's applyMaterialize has recorded its OrigAlloc in NewInvOf, so each
  // lock's receiver resolves via NewInvOf[ML.SourceEffect]. A single-object
  // escape point (no entry in MaxSeqForEscapePoint) emits its own locks here.
  auto EmitLock = [&](Value *Recv, Function *Callee,
                      ArrayRef<Value *> NonReceiverArgs) {
    if (!Callee)
      return;
    SmallVector<Value *, 4> Args;
    Args.push_back(Recv);
    for (Value *A : NonReceiverArgs)
      Args.push_back(A);
    CallInst *Enter = SB.CreateCall(Callee, Args);
    Enter->setCallingConv(CallingConv::Hotspot_JIT);
    // The re-emitted monitorenter is a REAL held lock on OrigAlloc (never a
    // deopt safepoint), so it MUST carry no "deopt" operand bundle — a bundle
    // here would describe a MATERIALIZED VO's lock as a safepoint state,
    // double-counting it against the deopt-bundle monitor section.
    assert(!Enter->hasOperandBundles() &&
           "re-emitted monitorenter must be bare");
  };
  // Lock lookup uses the ORIGINAL escape-point InsertBefore (the pre-scan-
  // captured key computeEscapePointLocks used), NOT the possibly eager-update-
  // re-aimed E.InsertBefore — a re-aimed Case-A materialize at a multi-object
  // interleaved-lock escape point would otherwise miss the key, fall to
  // per-effect emission, and mis-order the runtime lock stack.
  Instruction *LockKey = OrigInsertBefore.lookup(&E);
  if (!LockKey)
    LockKey = InsertBefore;
  auto MaxIt = Result.MaxSeqForEscapePoint.find(LockKey);
  if (MaxIt != Result.MaxSeqForEscapePoint.end()) {
    if (E.SeqNo == MaxIt->second) {
      auto It = Result.EscapePointLocks.find(LockKey);
      if (It != Result.EscapePointLocks.end()) {
        // Defensive depth-ordering check (the analogue of Graal
        // ObjectState.addLock's strictly-ascending guarantee): computeEscape
        // PointLocks sorts each escape point's MergedLock list ascending by
        // BytecodeDepth, and the emitted monitorenter sequence must match that
        // order so the runtime lock-nesting stack is rebuilt correctly. A
        // future sort regression or a keying asymmetry would otherwise mis-
        // order the locks silently. The enforced invariant here is NON-
        // DECREASING (the sort comparator is `A.Depth < B.Depth`, a weak
        // ordering): the merged list spans multiple sibling VOs, each
        // contributing its full captured lock stack, so two siblings holding
        // locks at the same depth produce equal-depth entries by design (e.g.
        // test 446 emits la,la,lb,lb). Graal's strictly-ascending per-
        // ObjectState guarantee is not directly checkable on this cross-object
        // merged list; the non-decreasing check still catches any unsorted/
        // reversed sequence (the sort-regression case that matters).
        bool First = true;
        uint32_t LastDepth = 0;
        for (const jeandle::MergedLock &ML : It->second) {
          assert((First || LastDepth <= ML.BytecodeDepth) &&
                 "emitted lock sequence must be non-decreasing in "
                 "BytecodeDepth (sort-regression guard; Graal "
                 "ObjectState.addLock per-object strict-ascending guarantee "
                 "is not checkable on the cross-object merged list)");
          First = false;
          LastDepth = ML.BytecodeDepth;
          auto NIt = NewInvOf.find(ML.SourceEffect);
          assert(NIt != NewInvOf.end() &&
                 "every sibling's OrigAlloc must be "
                 "recorded before the tail emits locks");
          EmitLock(NIt->second, ML.Callee, ML.NonReceiverArgs);
        }
      }
    }
    // A non-tail sibling emits nothing here; the tail emits the whole escape
    // point's locks.
  } else {
    // Per-effect (single-object / non-cascade) path: the analyzer's
    // captureMaterializedLocks sorts E.Locks ascending by BytecodeDepth with
    // the same weak ordering, so enforce non-decreasing here too.
    bool First = true;
    uint32_t LastDepth = 0;
    for (const jeandle::MaterializedLock &ML : E.Locks) {
      assert((First || LastDepth <= ML.BytecodeDepth) &&
             "emitted lock sequence must be non-decreasing in BytecodeDepth "
             "(sort-regression guard)");
      First = false;
      LastDepth = ML.BytecodeDepth;
      EmitLock(MatVal, ML.Callee, ML.NonReceiverArgs);
    }
  }

  // Only NewInvOf (set above) is needed: it is consumed by the lock re-emit's
  // per-object receiver resolution at multi-object escape points.
}

// Bundles the Function, the analysis result, and the shared per-apply state so
// each Effect subclass's apply() is self-contained (Jeandle's adaptation of
// Graal's `apply(StructuredGraph graph, ArrayList<Node> obsoleteNodes)` — LLVM
// mutates a Function, not a StructuredGraph).
//
// The shared maps are:
//   NewInvOf               effect -> OrigAlloc (CallBase). Used by the lock
//                          re-emit to resolve each sibling's receiver at an
//                          escape point that merges locks across objects.
//   InsertBeforeDependents escape-point InsertBefore -> the Materialize effects
//                          keyed on it. The eager-update hook
//                          (relocateDependentMaterializes) consumes this to
//                          re-aim each dependent Materialize to the in-block
//                          successor BEFORE a sibling erase nulls the
//                          WeakTrackingVH. Required for the Case-A path whose
//                          InsertBefore is a folded JavaOp invoke terminator
//                          that a sibling ReplaceCall erases (tests 438/439/440).
struct jeandle::TransformContext {
  Function &F;
  jeandle::PEAResult &Result;
  bool &Changed;

  // effect -> OrigAlloc (CallBase) it materializes onto. Filled incrementally
  // as each Materialize applies; consumed by the tail effect at a multi-object
  // escape point to resolve each MergedLock's receiver.
  DenseMap<const jeandle::MaterializeEffect *, CallBase *> &NewInvOf;

  // Reverse index: live InsertBefore -> Materialize effects keyed on it.
  DenseMap<Instruction *, SmallVector<jeandle::MaterializeEffect *, 4>>
      &InsertBeforeDependents;

  // effect -> its ORIGINAL escape-point InsertBefore (captured before Pass 1,
  // before any eager-update re-aim). The lock re-emit looks up
  // MaxSeqForEscapePoint/EscapePointLocks with this — the key
  // computeEscapePointLocks used — NOT the re-aimed E.InsertBefore, which
  // could miss the key at a multi-object escape point.
  DenseMap<const jeandle::MaterializeEffect *, Instruction *> &OrigInsertBefore;

  // OrigAllocs of PartiallyEscapes VOs. EliminateAllocation must SKIP these
  // (OrigAlloc is the single sound SSA value kept alive for the object's
  // surviving uses); only NeverEscapes OrigAllocs are erased. Built once in
  // run() from Result.EscapeClassification + Result.VirtualObjects.
  DenseSet<Instruction *> PartiallyEscapesAllocs;

  // ORIGINAL safepoint CallBase -> its latest rebuilt replacement, so multiple
  // RewriteDeoptBundleEffects at the same safepoint (a VO plus its transitive
  // members) accumulate descriptors on ONE rebuilt bundle. Each
  // RewriteDeoptBundleEffect::apply erases its CB and records the replacement
  // here; the next effect follows the chain. Keyed on the analysis-time
  // Safepoint pointer (stable identity; never dereferenced after erasure —
  // used only as a DenseMap key).
  DenseMap<CallBase *, CallBase *> SafepointReplacements = {};
};

void jeandle::ReplaceLoadEffect::apply(jeandle::TransformContext &Ctx) {
  if (!Target || !Replacement)
    return;
  Value *Repl = Replacement;
  // When the replacement's stamp is wider than the original, a Pi node would
  // normally be injected. LLVM has no per-Value stamp at this layer — the
  // closest analogue is the load-only metadata the original load may have
  // carried. Transfer those (only when both sides are LoadInsts and the
  // Replacement is missing the kind) so downstream LLVM passes do not lose the
  // narrower-than-default knowledge after RAUW.
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
  // The analyzer may have synthesized an unparented coercion instruction as the
  // replacement (a same-bit-width `bitcast` reinterpretation). Splice it, and
  // any still-unparented operand, in postorder so each operand is parented
  // before its user; all land immediately before Target. A PHINode replacement
  // is owned by a CreatePHI effect that runs LATER in SeqNo order, so it is
  // treated as a leaf here (splicing it mid-block is illegal).
  if (auto *RI = dyn_cast<Instruction>(Repl); RI && !isa<PHINode>(RI)) {
    SmallVector<Instruction *, 4> Stack;
    SmallPtrSet<Instruction *, 4> Visited;
    if (RI->getParent() == nullptr && Visited.insert(RI).second)
      Stack.push_back(RI);
    SmallVector<Instruction *, 4> PostOrder;
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
  }
  if (!Target->use_empty())
    Target->replaceAllUsesWith(Repl);
  // Eager-update: re-aim any Materialize keyed on `Target` to its next
  // instruction before the erase nulls the WeakTrackingVH. Loads are never
  // block terminators, so getNextNode() is the in-block successor.
  relocateDependentMaterializes(Ctx.InsertBeforeDependents, Target,
                                Target->getNextNode());
  Target->eraseFromParent();
  Ctx.Changed = true;
}

void jeandle::ReplaceCallEffect::apply(jeandle::TransformContext &Ctx) {
  // JavaOp folded against a virtual receiver: non-void results are replaced
  // with a constant and the call erased; void JavaOps use a null Replacement to
  // request deletion only. Folded results that feed `br i1` leave constant
  // terminators cleaned up by ConstantFoldTerminator in run().
  if (!Target)
    return;
  // foldGetClass records the constant Class mirror by oop id rather than as an
  // LLVM value: building the GC-safe oop-handle load here (instead of during
  // analysis) keeps the analyzer side-effect-free. RS4GC, which runs downstream
  // of PEA, treats the loaded addrspace(1) value as a managed pointer and
  // inserts relocates. The load is inserted before Target so it dominates every
  // use after the RAUW below.
  Value *Repl = Replacement;
  if (OopHandleId >= 0) {
    IRBuilder<> Builder(Target);
    Repl = createConstOopLoad(*Ctx.F.getParent(), Builder, OopHandleId);
  }
  if (Repl) {
    Target->replaceAllUsesWith(Repl);
  } else if (!Target->use_empty()) {
    return;
  }
  // For InvokeInst we cannot simply erase — the unwind edge must be dropped
  // first. JavaOp folds emit `call` (not invoke) calls in practice; defensively
  // handle invokes.
  if (auto *II = dyn_cast<InvokeInst>(Target)) {
    BasicBlock *Normal = II->getNormalDest();
    BasicBlock *Unwind = II->getUnwindDest();
    BasicBlock *Parent = II->getParent();
    Unwind->removePredecessor(Parent, /*KeepOneInputPHIs=*/true);
    BranchInst::Create(Normal, Parent);
    // Eager-update: re-aim any Materialize keyed on `II` to the freshly-created
    // `br` (II's normal successor in the SAME block) before erasing II. This
    // MUST use the `br` (II->getNextNode() after the Create), NOT
    // Normal->getFirstNonPHIOrDbg — the latter lives in the (multi-pred)
    // normal-dest block and would split the merge, replaying fields on every
    // predecessor's path (unsound).
    relocateDependentMaterializes(Ctx.InsertBeforeDependents, II,
                                  II->getNextNode());
    II->eraseFromParent();
  } else {
    // Eager-update: re-aim any Materialize keyed on `Target` to its next
    // instruction before the erase nulls the WeakTrackingVH.
    relocateDependentMaterializes(Ctx.InsertBeforeDependents, Target,
                                  Target->getNextNode());
    Target->eraseFromParent();
  }
  Ctx.Changed = true;
}

void jeandle::EliminateStoreEffect::apply(jeandle::TransformContext &Ctx) {
  if (!Target)
    return;
  // Eager-update (defensive, TODO(pea-deopt)): EliminateStore and Materialize-
  // at-store are mutually exclusive by the processStore dispatch, so this never
  // fires today, but a store CAN be a Materialize IP (value-side fall-through),
  // so the hook is future-proof if that exclusion ever changes.
  relocateDependentMaterializes(Ctx.InsertBeforeDependents, Target,
                                Target->getNextNode());
  Target->eraseFromParent();
  Ctx.Changed = true;
}

void jeandle::EliminateAllocationEffect::apply(jeandle::TransformContext &Ctx) {
  // A PartiallyEscapes VO keeps its OrigAlloc (it is the single sound SSA value
  // for surviving uses and carries the allocation-site deopt bundle). Only
  // NeverEscapes OrigAllocs are erased. The analyzer still emits
  // EliminateAllocation for PartiallyEscapes VOs; those become redundant and
  // are silently skipped here. A genuine misclassification (e.g. an
  // AlwaysEscapes VO, or a non-allocation Target) still fails loudly inside
  // eraseAllocation's existing isJeandleAllocation asserts — the skip here only
  // suppresses the case the model requires (PartiallyEscapes OrigAlloc kept
  // alive).
  if (Ctx.PartiallyEscapesAllocs.count(Target))
    return;
  if (eraseAllocation(Target))
    Ctx.Changed = true;
}

void jeandle::MaterializeEffect::apply(jeandle::TransformContext &Ctx) {
  // Replay field stores and re-emit locks onto OrigAlloc (kept alive for
  // PartiallyEscapes) — see applyMaterialize and the file header.
  applyMaterialize(Ctx.F, Ctx.Result, *this, Ctx.NewInvOf,
                   Ctx.OrigInsertBefore);
  Ctx.Changed = true;
}

void jeandle::CreatePHIEffect::apply(jeandle::TransformContext &Ctx) {
  // The analyzer's CreatePHI emission falls into two cases, distinguished by
  // RAUWOrigToPHI:
  //
  // (1) Materialized-object merge PHI (RAUWOrigToPHI == true, emitted ONLY by
  //     materializeAndBuildPhi): combines per-pred materializations of one
  //     VO's pointer at a mixed-state merge. OrigAlloc is the single SSA value
  //     on every path (it dominates every escape point), so this PHI is
  //     unnecessary: every incoming would be OrigAlloc and the PHI would
  //     trivially fold. SKIP creating it. The analyzer-built PhiInst stays
  //     unparented and is cleaned up by run()'s OwnedInsts sweep / the
  //     PEAResult destructor.
  //
  // (2) Field-value PHI (RAUWOrigToPHI == false, emitted by mergeFieldStates
  //     and synthesizeCaseC): merges a per-offset field VALUE (scalar or
  //     materialized-ref pointer) across preds / around a loop. This is NOT a
  //     materialized-object PHI — it tracks a real field value that must be
  //     merged. KEEP creating it. The analyzer's recorded
  //     (PHIIncomingValues[I], PHIIncomingBlocks[I]) are valid as-is: each
  //     incoming is a dominating field value, and a materialized-ref incoming
  //     is the peer VO's OrigAlloc, which is kept alive.
  if (RAUWOrigToPHI)
    return; // Case (1): skip the materialized-object merge PHI.

  // Case (2): insert the field-value PHI and wire its incomings directly.
  PHINode *Phi = PhiInst;
  assert(Phi && "CreatePHI effect requires a PhiInst");
  assert(Phi->getParent() == nullptr &&
         "CreatePHI's PhiInst must be unparented at apply time");
  Phi->insertBefore(Block->getFirstInsertionPt());
  assert(PHIIncomingValues.size() == PHIIncomingBlocks.size());
  for (unsigned I = 0; I < PHIIncomingValues.size(); ++I)
    Phi->addIncoming(PHIIncomingValues[I], PHIIncomingBlocks[I]);
  Ctx.Changed = true;
}

void jeandle::RewritePhiIncomingEffect::apply(jeandle::TransformContext &Ctx) {
  // No-op. This effect (emitted only by the Case-A path in processBlockPhis)
  // historically re-derived a carried DERIVED pointer (GEP/bitcast of a virtual
  // object) at the back-edge. OrigAlloc is KEPT (PartiallyEscapes), dominates
  // the body GEP, and the GEP stays valid; the carrying PHI's incoming is left
  // as the original IR value. Nothing to re-derive.
}

// Rewrite a safepoint's "deopt" operand bundle so a never-escaping virtual
// object referenced in it is described by a VO descriptor instead of a
// (soon-to-be-poisoned) OrigAlloc reference. Non-cfgKill (Pass 1): MUST run
// before Pass 2's EliminateAllocation RAUWs OrigAlloc to poison, otherwise
// the bundle operand would be poisoned (the analysis records this effect and
// the analyzer's generic escape path skips the handled bundle operand, so the
// object stays NeverEscapes and reaches Pass 2 with the bundle already
// rewritten).
void jeandle::RewriteDeoptBundleEffect::apply(jeandle::TransformContext &Ctx) {
  // The analyzer records this effect ONLY for a VO that is virtual at this
  // safepoint (recordDeoptBundleMappings gates on resolveVirtualRef =
  // ObjectState present & virtual). NeverEscapes VOs are virtual at every
  // safepoint; a PartiallyEscapes VO is virtual before its escape point
  // (described here) and materialized at/after it (NOT recorded → its real
  // OrigAlloc bundle operand is left untouched). So every recorded effect
  // becomes a descriptor — no classification guard. (AlwaysEscapes effects were
  // dropped by the analyzer.)
  //
  // Several RewriteDeoptBundleEffects may target the SAME safepoint (a VO plus
  // its transitive members). Each effect rebuilds the deopt bundle and erases
  // the prior CB; resolve the CURRENT CB by following the replacement chain
  // keyed on the analysis-time Safepoint (never dereferenced post-erasure —
  // used only as a DenseMap identity key).
  CallBase *CB = Safepoint;
  if (auto It = Ctx.SafepointReplacements.find(CB);
      It != Ctx.SafepointReplacements.end())
    CB = It->second;
  if (!CB || !CB->getParent())
    return; // safepoint erased (e.g. folded away) — TODO(pea-deopt).
  auto Deopt = CB->getOperandBundle(LLVMContext::OB_deopt);
  if (!Deopt)
    return; // bundle gone — nothing to rewrite.

  jeandle::VirtualObject &VObj = *Ctx.Result.VirtualObjects[ObjID];
  Value *OrigAlloc = VObj.AllocationCall;
  assert(OrigAlloc && "virtual VO missing OrigAlloc");
  uint64_t Klass = VObj.Klass;
  unsigned VObjID = ObjID;

  // Build the (basic_type, value) field pairs in declaration (byte-offset)
  // order. Each FieldValue is either Scalar (a plain scalar field) or
  // VirtualRef(InnerID) (a field referencing another in-scope VO, emitted as a
  // VORef field by id). The analyzer's greatest-fixpoint guarantees every
  // VORef target is itself described at this safepoint.
  SmallVector<MaterializeEffect::FieldEntry, 8> SortedFields(Fields);
  llvm::sort(SortedFields, [](const MaterializeEffect::FieldEntry &A,
                              const MaterializeEffect::FieldEntry &B) {
    return A.Offset < B.Offset;
  });
  SmallVector<VODescriptorField, 8> FieldPairs;
  for (const MaterializeEffect::FieldEntry &FE : SortedFields) {
    if (FE.Value.isVirtualRef()) {
      FieldPairs.push_back({FE.Offset, jeandle::T_OBJECT, /*IsVORef=*/true,
                            nullptr, FE.Value.getVirtualRef()});
    } else {
      assert(FE.Value.isScalar() &&
             "scoped deopt field must be Scalar or VirtualRef");
      jeandle::HotspotBasicType BT =
          jeandle::LLVM2JavaComputational(FE.Value.getDeclaredType());
      assert(BT != jeandle::T_ILLEGAL && "scoped deopt field has illegal type");
      FieldPairs.push_back(
          {FE.Offset, BT, /*IsVORef=*/false, FE.Value.getScalar(), 0});
    }
  }

  // The VO descriptor section sits AFTER the duplicated-BCI marker and BEFORE
  // the locals section. Anything before InsertPos (prefix + BCI pair) is
  // preserved verbatim.
  unsigned InsertPos = getDeoptScopeVOInsertPos(*CB);

  // Confirm OrigAlloc is still a bundle input (it may have been scrubbed by an
  // earlier transform step). A ROOT (OrigAllocInBundle) requires OrigAlloc
  // present — bail to avoid an orphan descriptor whose slot is already gone.
  // A TRANSITIVE member (OrigAllocInBundle=false) is referenced only via
  // another VO's VORef field, so its OrigAlloc is never a bundle operand —
  // proceed and emit the descriptor unconditionally.
  bool OrigAllocPresent = false;
  for (unsigned i = InsertPos; i < Deopt->Inputs.size(); ++i)
    if (Deopt->Inputs[i].get() == OrigAlloc) {
      OrigAllocPresent = true;
      break;
    }
  if (!OrigAllocPresent && OrigAllocInBundle)
    return;

  LLVMContext &CtxV = CB->getContext();

  SmallVector<Value *, 16> Args;
  Args.reserve(Deopt->Inputs.size() + 4 + FieldPairs.size() * 2);
  IRBuilder<> B(CtxV);

  // 1. prefix + duplicated-BCI pair.
  for (unsigned i = 0; i < InsertPos; ++i)
    Args.push_back(Deopt->Inputs[i].get());

  // 2. VO descriptor (ScalarValueType header + klass + field_count + fields).
  // The header basicType is T_ARRAY for an array VO (HotSpot's parser
  // dispatches on it to rebuild a uniform array rather than walk an
  // InstanceKlass layout) and T_OBJECT for an instance VO.
  appendVirtualObjectDescriptor(Args, B, Klass, VObjID, VObj.isArray(),
                                FieldPairs);

  // 3. remainder (locals/stack/monitors/orig_pc): copy verbatim, but each
  //    slot whose value is OrigAlloc becomes a VORefType reference
  //    (encoding + vo_id). The preceding appended entry was that slot's
  //    encoding; pop it and emit the VORef slot in its place. OrigAlloc is a
  //    pointer, so it only ever appears at a value position (preceded by its
  //    ConstantInt encoding), never at an encoding position.
  //
  //    A MONITOR entry whose owner is OrigAlloc (a PEA-eliminated lock on this
  //    virtual VO) is rewritten in place to {enc(MonitorType, index=1), vo-id,
  //    basic_lock} — the owner becomes a VORef by vo-id and the lock is marked
  //    eliminated so HotSpot relock_objects re-acquires it on the realloc'd
  //    owner. The basic_lock slot (the bundle input AFTER the owner) is
  //    preserved verbatim; ObjectSynchronizer::enter initializes it at relock,
  //    so a never-written (folded-monitorenter) slot is safe. A real (non-
  //    eliminated) lock on a MATERIALIZED VO has no RewriteDeoptBundleEffect
  //    (its OrigAlloc is never rewritten), so it is left untouched here — the
  //    standard eliminated=false, owner=OrigAlloc path.
  for (unsigned i = InsertPos; i < Deopt->Inputs.size(); ++i) {
    Value *V = Deopt->Inputs[i].get();
    if (V == OrigAlloc) {
      assert(i > InsertPos && !Args.empty() &&
             "OrigAlloc slot missing its preceding encoding");
      assert(isa<ConstantInt>(Args.back()) &&
             "deopt slot encoding must be a ConstantInt");
      uint64_t SlotEnc = cast<ConstantInt>(Args.back())->getZExtValue();
      auto SlotVT = jeandle::DeoptValueEncoding::decode(SlotEnc).valueType();
      if (SlotVT == jeandle::DeoptValueEncoding::MonitorType) {
        // Eliminated-lock monitor: owner = VORef(vo-id), eliminated=true.
        // index=1 in the monitor encoding is the eliminated-VORef-owner tag
        // (see DeoptValueEncoding::MonitorType in Deoptimization.h).
        Args.pop_back();
        Constant *MonEnc = ConstantInt::get(
            Type::getInt64Ty(CtxV),
            jeandle::DeoptValueEncoding(
                /*Index=*/1, jeandle::DeoptValueEncoding::MonitorType,
                jeandle::T_OBJECT)
                .encode());
        Args.push_back(MonEnc);
        Args.push_back(B.getInt32(VObjID));
      } else {
        // Preserve the slot's Local/Stack identity so the HotSpot parser
        // routes the VORef to the correct interpreter array (locals vs
        // expression stack); the parser routes every entry by encoding type,
        // so a single VORef type would misroute a VO-ref living in a stack
        // slot.
        assert((SlotVT == jeandle::DeoptValueEncoding::LocalType ||
                SlotVT == jeandle::DeoptValueEncoding::StackType) &&
               "OrigAlloc deopt slot must be Local/Stack/Monitor");
        Args.pop_back();
        auto RefVT = (SlotVT == jeandle::DeoptValueEncoding::LocalType)
                         ? jeandle::DeoptValueEncoding::VORefLocalType
                         : jeandle::DeoptValueEncoding::VORefStackType;
        Constant *VORefEnc = ConstantInt::get(
            Type::getInt64Ty(CtxV),
            jeandle::DeoptValueEncoding(VObjID, RefVT, jeandle::T_OBJECT)
                .encode());
        Args.push_back(VORefEnc);
        Args.push_back(B.getInt32(VObjID));
      }
    } else {
      Args.push_back(V);
    }
  }

  // 4. Rebuild the safepoint with the new "deopt" bundle. CallBase::Create
  //    clones CB with the bundle matching the tag replaced and every other
  //    bundle (funclet, gc-transition, ...) preserved; the clone is inserted
  //    before CB, then CB is RAUW'd and erased (same pattern as
  //    CFGuard/GlobalOpt). NewCB lands in CB's block, so its normal/unwind
  //    dests keep CB's block as a predecessor — no PHI unwiring is needed.
  CallBase *NewCB =
      CallBase::Create(CB, OperandBundleDef("deopt", Args), CB->getIterator());
  NewCB->takeName(CB);
  CB->replaceAllUsesWith(NewCB);
  CB->eraseFromParent();
  // Record the replacement keyed on the ORIGINAL Safepoint so a subsequent
  // RewriteDeoptBundleEffect at this safepoint resolves NewCB (CB has been
  // erased above).
  Ctx.SafepointReplacements[Safepoint] = NewCB;
  Ctx.Changed = true;
}

// Apply every effect where isCfgKill()==CfgKills, in SeqNo order (Jeandle's
// substitute for Graal's list-order — see Effect::SeqNo). Pass 1 calls this
// with CfgKills=false (every effect except EliminateAllocation); Pass 2 with
// CfgKills=true (EliminateAllocation only).
void jeandle::EffectList::apply(jeandle::TransformContext &Ctx, bool CfgKills) {
  SmallVector<jeandle::Effect *, 16> Order;
  Order.reserve(Effects.size());
  for (auto &E : Effects)
    Order.push_back(E.get());
  llvm::sort(Order, [](const jeandle::Effect *A, const jeandle::Effect *B) {
    return A->SeqNo < B->SeqNo;
  });
  for (jeandle::Effect *E : Order)
    if (E->isCfgKill() == CfgKills)
      E->apply(Ctx);
}

PreservedAnalyses PartialEscapeTransform::run(Function &F,
                                              FunctionAnalysisManager &FAM) {
  // Gate on jeandle.java_method_compilation.
  Module *M = F.getParent();
  if (!M || !M->getNamedMetadata(jeandle::Metadata::JavaMethodCompilation))
    return PreservedAnalyses::all();

  auto &Result = FAM.getResult<PartialEscapeAnalysis>(F);
  if (!Result.hasOptimizationOpportunity())
    return PreservedAnalyses::all();

  bool Changed = false;

  // Build the set of PartiallyEscapes OrigAllocs. These are kept alive
  // (EliminateAllocation skips them) because OrigAlloc is the single sound SSA
  // value for a PartiallyEscapes VO and carries the allocation-site deopt
  // bundle. NeverEscapes OrigAllocs are erased (the VO is described by a
  // descriptor in the deopt bundle and HotSpot reallocs at deopt).
  DenseSet<Instruction *> PartiallyEscapesAllocs;
  for (const auto &Kv : Result.EscapeClassification) {
    if (Kv.second != jeandle::PEAResult::EscapeKind::PartiallyEscapes)
      continue;
    if (Kv.first >= Result.VirtualObjects.size())
      continue;
    if (auto *Alloc = Result.VirtualObjects[Kv.first]->AllocationCall)
      PartiallyEscapesAllocs.insert(Alloc);
  }

  // effect -> OrigAlloc (the CallBase each Materialize replays onto). Filled
  // incrementally as each Materialize applies; consumed by the tail effect at
  // a multi-object escape point to resolve each MergedLock's receiver.
  DenseMap<const jeandle::MaterializeEffect *, CallBase *> NewInvOf;

  ReversePostOrderTraversal<Function *> RPOT(&F);

  // Build the per-escape-point merged lock lists (one global depth-sort per
  // materialize point) before Pass 1 applies effects. Re-entrant interleaved
  // lock stacks across objects at one escape point MUST be re-emitted as ONE
  // globally depth-sorted list (per-object emission would mis-order them on
  // the runtime lock stack).
  Result.computeEscapePointLocks();

  // Eager-update reverse index: for each Materialize effect, record its live
  // InsertBefore instruction -> effect list. A sibling erase
  // (ReplaceLoad/ReplaceCall/EliminateStore) consults this to re-aim each
  // dependent Materialize to the in-block successor before nulling the
  // WeakTrackingVH. Required for the Case-A path whose InsertBefore is a folded
  // JavaOp invoke terminator (tests 438/439/440) — see
  // relocateDependentMaterializes.
  DenseMap<Instruction *, SmallVector<jeandle::MaterializeEffect *, 4>>
      InsertBeforeDependents;
  // Each Materialize effect's ORIGINAL escape-point InsertBefore, captured here
  // before Pass 1 (before any eager-update re-aim). computeEscapePointLocks
  // keys EscapePointLocks / MaxSeqForEscapePoint by this same analysis-time
  // instruction, so the lock re-emit must look up with it — NOT with
  // E.InsertBefore, which relocateDependentMaterializes may re-aim to the
  // in-block successor for a Case-A materialize. Using the re-aimed value would
  // miss the key, fall to per-effect emission, and mis-order multi-object
  // interleaved locks.
  DenseMap<const jeandle::MaterializeEffect *, Instruction *> OrigInsertBefore;
  for (auto &Kv : Result.BlockEffects)
    for (jeandle::Effect &E : Kv.second) {
      auto *M = dyn_cast<jeandle::MaterializeEffect>(&E);
      if (!M)
        continue;
      if (Instruction *Key = dyn_cast_or_null<Instruction>(M->InsertBefore)) {
        InsertBeforeDependents[Key].push_back(M);
        OrigInsertBefore.try_emplace(M, Key);
      }
    }

  // -------------------------------------------------------------------------
  // Pass 1: non-cfgKill effects (ReplaceLoad, ReplaceCall, EliminateStore,
  // Materialize, CreatePHI) — applied per-block in RPO via EffectList::apply,
  // which sorts by SeqNo and dispatches each effect's apply() through
  // TransformContext (Jeandle's analog of Graal's
  // apply(graph, obsoleteNodes, cfgKills=false)). isCfgKill() partitions the
  // two passes; EliminateAllocation is the only cfgKill, so it is skipped here.
  // -------------------------------------------------------------------------
  jeandle::TransformContext Ctx{
      F, Result, Changed, NewInvOf, InsertBeforeDependents, OrigInsertBefore};
  Ctx.PartiallyEscapesAllocs = std::move(PartiallyEscapesAllocs);

  for (BasicBlock *BB : RPOT) {
    auto It = Result.BlockEffects.find(BB);
    if (It == Result.BlockEffects.end())
      continue;
    It->second.apply(Ctx, /*CfgKills=*/false);
  }

  // -------------------------------------------------------------------------
  // Pass 2: cfgKill effects (EliminateAllocation only) — same dispatch with
  // CfgKills=true. NeverEscapes OrigAllocs are erased here; PartiallyEscapes
  // OrigAllocs are skipped (Ctx.PartiallyEscapesAllocs) and stay alive.
  // -------------------------------------------------------------------------
  for (BasicBlock *BB : RPOT) {
    auto It = Result.BlockEffects.find(BB);
    if (It == Result.BlockEffects.end())
      continue;
    It->second.apply(Ctx, /*CfgKills=*/true);
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

  // Fold trivial PHIs. No materialized-object PHI is created (OrigAlloc is the
  // single value), but field-value PHIs created by CreatePHIEffect can still
  // collapse to a single value when every incoming agrees (e.g. a loop field-PHI
  // whose only back-edge incoming is the same value as the preheader incoming),
  // and nested loops can leave dead PHI cycles. PHINode::hasConstantValue
  // collapses both phi(X,X) and phi(self, X) to X; iterate to fixpoint so a
  // fold that makes an enclosing phi trivial is caught. The trivially-dead
  // sweep below cannot break a PHI cycle (each phi is "used" by the next), so
  // this runs first. (Mirrors downstream GVN/InstCombine; doing it here keeps
  // PEA output clean.)
  bool FoldedPhi = true;
  while (FoldedPhi) {
    FoldedPhi = false;
    for (BasicBlock &BB : F) {
      for (Instruction &I : llvm::make_early_inc_range(BB)) {
        auto *PN = dyn_cast<PHINode>(&I);
        if (!PN || PN->getNumIncomingValues() == 0)
          continue;
        if (Value *V = PN->hasConstantValue()) {
          PN->replaceAllUsesWith(V);
          PN->eraseFromParent();
          FoldedPhi = true;
        }
      }
    }
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
