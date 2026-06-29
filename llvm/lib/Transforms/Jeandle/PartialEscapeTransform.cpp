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
//   Materialize, CreatePHI — applied per-block in RPO via EffectList::apply,
//   which sorts by SeqNo and dispatches each effect's apply() through
//   TransformContext.
//
//   Pass 2 (cfgKill): EliminateAllocation — rewrites an invoke alloc into an
//   unconditional branch to the normal dest (dropping the unwind edge), or
//   plain-erases a call alloc. isCfgKill() is true ONLY for this effect
//   (matching Graal's deleteNode(WithExceptionNode)/killIfBranch).
//
// Between the passes a point-sensitive resolution sub-pass rewrites each
// surviving original-allocation use to the materialize NewInv / merge PHI
// that dominates it (Jeandle's analog of Graal's per-point `aliases` map).
// After both passes: ConstantFoldTerminator, a trivially-dead sweep, and
// EliminateUnreachableBlocks.
//
// At each escape point MaterializeEffect::apply emits a new Hotspot_JIT
// InvokeInst, replays tracked field stores, and re-emits surviving
// monitorenters; EliminateAllocation in Pass 2 then erases the now-dead
// original. The invoke reuses the original alloc's unwind dest when the
// original was itself an invoke, else synthesizes a minimal landingpad+resume
// block, and tags the return with java-klass, java-klass-exact, and nonnull.
//
// Lock cascade: when an object escapes with locks held, the lock model DELETES
// the folded monitorenters and captures the surviving unbalanced enters into
// MaterializeEffect::Locks (sorted ascending by bytecode depth), re-emitting
// them right after the new allocation (Graal: synthetic MonitorEnterNodes at
// the CommitAllocationNode). Matching downstream exits survive in IR with
// operands RAUW'd onto the new invoke.
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
#include "llvm/IR/Dominators.h"
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
// containing block at the MaterializeEffect's InsertBefore so the new
// materialization is the terminator, emit a hotspotcc InvokeInst, replay
// tracked field stores at the top of the normal-dest block, and record the
// materialization (OrigAlloc → NewInv) in Defs for the point-sensitive
// resolution sub-pass (OrigAlloc is not RAUW'd inline). The same OrigAlloc may
// be materialized multiple times (mixed-state merge synthesizing a per-pred
// materialization on each virtual incoming): record each (analyzer-recorded-
// pred-block, OrigAlloc) → NewInv in MatPerBlock (CreatePHI picks the right
// per-incoming NewInv) and Origin → MatCont in BlockRename (so the PHI's
// incoming-block names the post-split merge-pred). Lock cascade: the surviving
// unbalanced enters were captured by the analyzer into the MaterializeEffect's
// Locks (sorted ascending by bytecode depth) and are re-emitted right after
// the field stores here.
//
// The materialized invoke is structurally identical to a frontend allocation
// site (hotspotcc `jeandle.new_instance` / `jeandle.new_array`, addrspace(1)
// return, exception edge), so the downstream GC-statepoint pipeline
// (PEA → InsertGCBarriers → ... → RewriteStatepointsForGC) wraps it
// uniformly with gc.statepoint/gc.result/gc.relocate; splitBasicBlock is
// SSA-preserving and the materialized pointer dominates every use in MatCont.
// See `partial-escape/310_full_pipeline_statepoint.ll`.
static void applyMaterialize(Function &F, const jeandle::PEAResult &Result,
                             const jeandle::MaterializeEffect &E,
                             DenseMap<Value *, Value *> &NewAllocFor,
                             DenseMap<std::pair<BasicBlock *, Value *>, Value *>
                                 &MatPerBlock,
                             DenseMap<BasicBlock *, BasicBlock *> &BlockRename,
                             DenseMap<Value *, SmallVector<Value *, 4>> &Defs) {
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
      VObj.isInstance() ? "jeandle.new_instance" : "jeandle.new_array";
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
  // holds OrigAlloc), which the resolution sub-pass would rewrite to NewInv —
  // a self-reference the verifier rejects.
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

  // Re-emit surviving monitorenters at the materialize point (Graal analog:
  // synthetic MonitorEnterNodes attached to the CommitAllocationNode during
  // lowering, sorted ascending by lock depth). The analyzer deleted the
  // original enters and captured each here as a self-contained MaterializedLock
  // (callee + non-receiver args + bytecode depth). Clone each with the freshly
  // materialized pointer as receiver, after the field stores (object fully
  // initialized before the lock is acquired) and before the escape instruction.
  // Mechanism note (#13): Graal lowers a fresh MonitorEnterNode carrying the
  // original MonitorId; Jeandle clones the original callee + BasicLock args,
  // receiver = NewInv — behaviorally equivalent, the LLVM-IR form of the same
  // lock acquisition at the materialize point.
  for (const jeandle::MaterializedLock &ML : E.Locks) {
    if (!ML.Callee)
      continue;
    SmallVector<Value *, 4> Args;
    Args.push_back(NewInv);
    for (Value *A : ML.NonReceiverArgs)
      Args.push_back(A);
    CallInst *Enter = SB.CreateCall(ML.Callee, Args);
    Enter->setCallingConv(CallingConv::Hotspot_JIT);
  }

  // Record this materialization in NewAllocFor so any later applyMaterialize
  // can rewrite a recorded MaterializedRef referencing OrigAlloc to the live
  // NewInv (field-store replay of a nested virtual).
  NewAllocFor[OrigAlloc] = NewInv;
  // Per-pred mapping for CreatePHI to pick the right NewInv per incoming.
  // Also record the block split so the PHI's incoming-block resolves to the
  // post-split MatCont.
  MatPerBlock[{AnalyzerRecordedPred, OrigAlloc}] = NewInv;
  // Per-pred-distinct placeholder resolution (Graal: a distinct
  // AllocatedObjectNode per materialize). When the analyzer carried a per-pred
  // placeholder Value* (one per (pred, ObjectID)), record the mapping under
  // that placeholder so CreatePHI incomings and any inherited materialized
  // value resolve to THIS pred's own NewInv — never the global last-write-wins
  // NewAllocFor[OrigAlloc]. The placeholder is never inserted into IR; it is
  // only ever looked up here.
  if (E.PerPredPlaceholder) {
    MatPerBlock[{AnalyzerRecordedPred, E.PerPredPlaceholder}] = NewInv;
    NewAllocFor[E.PerPredPlaceholder] = NewInv;
  }
  BlockRename[Origin] = MatCont;

  // Record this NewInv as a definition point of OrigAlloc. OrigAlloc is not
  // RAUW'd inline — the point-sensitive resolution sub-pass (run after
  // Pass 1, once the CFG is stable and a fresh DominatorTree is available)
  // rewrites each surviving OrigAlloc use to the unique dominating def (this
  // NewInv, a sibling per-pred NewInv, or a merge PHI). This mirrors Graal's
  // per-point alias resolution (getAlias/getAliasAndResolve) and is what makes
  // escape-point (non-dominating) materialization SSA-sound. Deopt-bundle
  // operands are scrubbed to a typed null in the sub-pass (PEA stays
  // deopt-agnostic; see resolveMaterializedUses).
  Defs[OrigAlloc].push_back(NewInv);
}

// Bundles the Function, the analysis result, and the shared per-apply maps so
// each Effect subclass's apply() is self-contained (Jeandle's adaptation of
// Graal's `apply(StructuredGraph graph, ArrayList<Node> obsoleteNodes)` — LLVM
// mutates a Function, not a StructuredGraph, and carries the alias/def maps the
// analysis could not populate because it cannot mutate IR).
struct jeandle::TransformContext {
  Function &F;
  jeandle::PEAResult &Result;
  DenseMap<Value *, Value *> &NewAllocFor;
  DenseMap<std::pair<BasicBlock *, Value *>, Value *> &MatPerBlock;
  DenseMap<BasicBlock *, BasicBlock *> &BlockRename;
  DenseMap<Value *, SmallVector<Value *, 4>> &Defs;
  bool &Changed;
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
    // Defensive: if the coercion's operand happens to be an OrigAlloc that's
    // been materialized, redirect through NewAllocFor.
    auto It2 = Ctx.NewAllocFor.find(RI);
    if (It2 != Ctx.NewAllocFor.end())
      Repl = It2->second;
  }
  if (!Target->use_empty())
    Target->replaceAllUsesWith(Repl);
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
  if (Replacement) {
    Target->replaceAllUsesWith(Replacement);
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
    II->eraseFromParent();
  } else {
    Target->eraseFromParent();
  }
  Ctx.Changed = true;
}

void jeandle::EliminateStoreEffect::apply(jeandle::TransformContext &Ctx) {
  if (!Target)
    return;
  Target->eraseFromParent();
  Ctx.Changed = true;
}

void jeandle::EliminateAllocationEffect::apply(jeandle::TransformContext &Ctx) {
  if (eraseAllocation(Target))
    Ctx.Changed = true;
}

void jeandle::MaterializeEffect::apply(jeandle::TransformContext &Ctx) {
  // Emit the materialization sequence. The original allocation's uses are
  // resolved later by the point-sensitive resolution sub-pass (not RAUW'd
  // inline); the allocation itself is erased by EliminateAllocation in Pass 2.
  applyMaterialize(Ctx.F, Ctx.Result, *this, Ctx.NewAllocFor, Ctx.MatPerBlock,
                   Ctx.BlockRename, Ctx.Defs);
  Ctx.Changed = true;
}

void jeandle::CreatePHIEffect::apply(jeandle::TransformContext &Ctx) {
  // Insert the unparented PHI created by the analyzer at the head of the merge
  // block (after any existing PHIs), and wire up its incoming values. For each
  // incoming (V, Pred): walk BlockRename to the live merge-pred; if V refers to
  // an OrigAlloc materialized at this (Pred, V), use the per-pred NewInv from
  // MatPerBlock, else fall back to NewAllocFor.
  PHINode *Phi = PhiInst;
  assert(Phi && "CreatePHI effect requires a PhiInst");
  assert(Phi->getParent() == nullptr &&
         "CreatePHI's PhiInst must be unparented at apply time");
  BasicBlock *MergeBB = Block;
  Phi->insertBefore(MergeBB->getFirstInsertionPt());
  assert(PHIIncomingValues.size() == PHIIncomingBlocks.size());
  for (unsigned I = 0; I < PHIIncomingValues.size(); ++I) {
    Value *V = PHIIncomingValues[I];
    BasicBlock *Pred = PHIIncomingBlocks[I];
    if (auto *VI = dyn_cast<Instruction>(V)) {
      auto It = Ctx.MatPerBlock.find({Pred, VI});
      if (It != Ctx.MatPerBlock.end()) {
        V = It->second;
      } else {
        auto It2 = Ctx.NewAllocFor.find(VI);
        if (It2 != Ctx.NewAllocFor.end())
          V = It2->second;
      }
    }
    // Resolve the live pred BB through BlockRename.
    BasicBlock *LivePred = Pred;
    while (true) {
      auto It = Ctx.BlockRename.find(LivePred);
      if (It == Ctx.BlockRename.end())
        break;
      LivePred = It->second;
    }
    Phi->addIncoming(V, LivePred);
  }
  // Register the freshly built PHI as a definition point of OrigAlloc so the
  // point-sensitive resolution sub-pass rewrites post-merge OrigAlloc uses onto
  // it (the dominance check restricts the rewrite to uses the PHI dominates).
  if (RAUWOrigToPHI && ObjID != jeandle::InvalidObjectID) {
    jeandle::VirtualObject &VObj = *Ctx.Result.VirtualObjects[ObjID];
    if (VObj.AllocationCall)
      Ctx.Defs[VObj.AllocationCall].push_back(Phi);
  }
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

// Point-sensitive resolution of original-allocation uses — Jeandle's analog of
// Graal's per-point alias resolution (the `aliases` map / getAlias /
// getAliasAndResolve, which Graal maintains because it REPLACES the allocation
// node with a VirtualObjectNode during analysis). LLVM's Analysis/Transform
// split forbids mutating IR during analysis, so OrigAlloc persists as a real
// invoke until here; this pass makes its ROLE Graal-equivalent by resolving
// every surviving use to the def that dominates it, then Pass 2 erases the
// now-use-less allocation.
//
// Run AFTER Pass 1 (which has placed every materialize NewInv and merge PHI and
// settled the CFG via block splits), so a freshly-computed DominatorTree is
// valid. For each OrigAlloc use: pick the unique def in Defs[OrigAlloc] that
// dominates it. PEA guarantees uniqueness per use-path — an arm's NewInv
// dominates only that arm's pre-merge uses; a merge PHI dominates only
// post-merge uses (no single arm dominates a real multi-pred merge). Deopt-
// bundle operands are scrubbed to a typed null (PEA stays deopt-agnostic).
static void resolveMaterializedUses(
    Function &F, DenseMap<Value *, SmallVector<Value *, 4>> &Defs) {
  if (Defs.empty())
    return;
  DominatorTree DT(F);

  for (auto &Kv : Defs) {
    Value *OrigAlloc = Kv.first;
    const SmallVector<Value *, 4> &DefList = Kv.second;
    if (OrigAlloc->use_empty())
      continue;
    Value *NullVO =
        ConstantPointerNull::get(cast<PointerType>(OrigAlloc->getType()));
    for (Use &U : llvm::make_early_inc_range(OrigAlloc->uses())) {
      // Find the unique dominating definition.
      Value *Dom = nullptr;
      for (Value *Def : DefList)
        if (DT.dominates(Def, U)) {
          Dom = Def;
          break;
        }
      if (!Dom)
        continue; // no dominating def; leave for Pass 2's poison RAUW.
      // Scrub deopt-bundle operands to a typed null rather than threading a
      // (possibly non-dominating) NewInv/PHI into a sibling's deopt bundle.
      if (auto *CB = dyn_cast<CallBase>(U.getUser())) {
        unsigned OpIdx = U.getOperandNo();
        if (CB->isBundleOperand(OpIdx) &&
            CB->getOperandBundleForOperand(OpIdx).isDeoptOperandBundle()) {
          U.set(NullVO);
          continue;
        }
      }
      U.set(Dom);
    }
  }
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
  // Per-OrigAlloc definition points (every materialize NewInv + every merge
  // PHI) populated during Pass 1. Consumed by resolveMaterializedUses after
  // Pass 1 to rewrite each surviving OrigAlloc use to its dominating def.
  DenseMap<Value *, SmallVector<Value *, 4>> Defs;

  // -------------------------------------------------------------------------
  // PRE-PASS: split critical edges before per-pred materialisation.
  //
  // A per-pred Materialize replaces PH's terminator with a materialisation
  // invoke carrying an OOM unwind edge. If PH has multiple successors the OOM
  // would become observable on every PH→* edge — a Java-semantics change. So
  // for each IsPerPred Materialize on a PH with >1 successor, split the
  // PH→S edge where S is the merge that requested the per-pred mat (S has
  // >1 pred and BlockEffects[S] has a CreatePHI naming PH), then re-aim the
  // per-pred Materialize effects onto the new edge-block PH' and seed
  // BlockRename[PH] = PH' so CreatePHI's BlockRename-chain walk routes the
  // analyzer-recorded PH incoming through PH' to MatCont.
  {
    struct EdgeKey {
      BasicBlock *PH;
      unsigned SuccIdx;
    };
    SmallVector<EdgeKey, 4> Splits;
    DenseSet<std::pair<BasicBlock *, BasicBlock *>> SeenEdges;
    for (const auto &KvOut : Result.BlockEffects) {
      for (const auto &E : KvOut.second) {
        const auto *M = dyn_cast<jeandle::MaterializeEffect>(&E);
        if (!M || !M->IsPerPred)
          continue;
        BasicBlock *PH = E.Block;
        if (!PH)
          continue;
        Instruction *Term = PH->getTerminator();
        if (!Term || Term->getNumSuccessors() <= 1)
          continue;
        // The critical-edge split is unconditional: the lock model deletes the
        // original monitorenter and re-emits it at the materialize point, so no
        // surviving enter sits in PH that would lose its dominating receiver if
        // the Materialize moved to a new edge block.
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
            const auto *CPE = dyn_cast<jeandle::CreatePHIEffect>(&PE);
            if (!CPE)
              continue;
            for (BasicBlock *IB : CPE->PHIIncomingBlocks) {
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
    // Re-aim per-pred Materialize effects from their original PH bucket onto
    // the new edge-block. Also move the BlockEffects entry so the RPO walk
    // applies the effects at the correct block boundary. The Stay effects
    // remain in the Old bucket; each Move effect is spliced out, re-aimed via
    // the MaterializeEffect setters, and added to the New bucket.
    if (!PHRename.empty()) {
      for (const auto &KvOut : PHRename) {
        BasicBlock *Old = KvOut.first;
        BasicBlock *New = KvOut.second;
        auto It = Result.BlockEffects.find(Old);
        if (It == Result.BlockEffects.end())
          continue;
        jeandle::EffectList &Src = It->second;
        jeandle::EffectList Move;
        size_t I = 0;
        while (I < Src.size()) {
          jeandle::Effect &E = Src[I];
          auto *M = dyn_cast<jeandle::MaterializeEffect>(&E);
          if (M && M->IsPerPred && E.Block == Old) {
            M->setBlock(New);
            M->setInsertBefore(&*New->getFirstNonPHIOrDbg());
            Move.add(Src.spliceOut(I));
          } else {
            ++I;
          }
        }
        Result.BlockEffects[New].addAll(Move);
      }
    }
  }

  // Recompute RPOT AFTER any critical-edge splits so the new edge-blocks
  // are visited in Pass 1.
  ReversePostOrderTraversal<Function *> RPOT(&F);

  // -------------------------------------------------------------------------
  // Pass 1: non-cfgKill effects (ReplaceLoad, ReplaceCall, EliminateStore,
  // Materialize, CreatePHI) — applied per-block in RPO via EffectList::apply,
  // which sorts by SeqNo and dispatches each effect's apply() through
  // TransformContext (Jeandle's analog of Graal's
  // apply(graph, obsoleteNodes, cfgKills=false)). isCfgKill() partitions the
  // two passes; EliminateAllocation is the only cfgKill, so it is skipped here.
  // -------------------------------------------------------------------------
  jeandle::TransformContext Ctx{F, Result,        NewAllocFor,
                                MatPerBlock, BlockRename,   Defs,
                                Changed};
  for (BasicBlock *BB : RPOT) {
    auto It = Result.BlockEffects.find(BB);
    if (It == Result.BlockEffects.end())
      continue;
    It->second.apply(Ctx, /*CfgKills=*/false);
  }

  // -------------------------------------------------------------------------
  // Resolution sub-pass: rewrite each surviving OrigAlloc use to its dominating
  // materialize NewInv / merge PHI (Jeandle's analog of Graal's point-sensitive
  // alias resolution). Runs after Pass 1 has settled the CFG; before Pass 2 so
  // the allocation becomes use-empty before EliminateAllocation erases it.
  // -------------------------------------------------------------------------
  if (!Defs.empty()) {
    resolveMaterializedUses(F, Defs);
    Changed = true;
  }

  // -------------------------------------------------------------------------
  // Pass 2: cfgKill effects (EliminateAllocation only) — same dispatch with
  // CfgKills=true. Runs after the resolution sub-pass so each allocation is
  // use-empty before it is erased.
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
