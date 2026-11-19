//===- SafepointPollElimination.cpp - Safepoint poll elimination ---------===//
//
// Copyright (c) 2026, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Jeandle/SafepointPollElimination.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Jeandle/JeandleUtils.h"
#include "llvm/IR/Jeandle/Metadata.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Jeandle/SafepointUtils.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"

using namespace llvm;

#define DEBUG_TYPE "safepoint-poll-elimination"

namespace {

using jeandle::isSafepoint;
using jeandle::isSafepointPoll;
using jeandle::RequiredPolls;

static cl::opt<bool> EnableBranchPollCoalescing(
    "jeandle-enable-branch-poll-coalescing", cl::Hidden, cl::init(false),
    cl::desc("Allow poll coalescing across branches and merges before O3."));

[[maybe_unused]] StringRef modeName(SafepointPollEliminationMode Mode) {
  switch (Mode) {
  case SafepointPollEliminationMode::Early:
    return "early";
  case SafepointPollEliminationMode::AfterStripMining:
    return "after-strip-mining";
  case SafepointPollEliminationMode::LoopDeletionPrep:
    return "loop-deletion-prep";
  }
  llvm_unreachable("unknown SafepointPollEliminationMode");
}

// Keep proofs within one loop iteration and one ownership region. Later loop
// deletion or strip mining can then change a child without invalidating an
// outside poll's replacement. Cutting backedges also prevents circular proofs.
bool canPropagateCoverage(BasicBlock *From, BasicBlock *To, LoopInfo &LI,
                          DominatorTree &DT) {
  if (LI.getLoopFor(From) != LI.getLoopFor(To) || DT.dominates(To, From))
    return false;
  if (EnableBranchPollCoalescing)
    return true;
  // Retain branch-local side effects by default. Removing them can expose
  // speculation of both arms and lengthen dependent computations after O3.
  // A returning invoke still covers its unique normal continuation.
  if (To->getSinglePredecessor() != From)
    return false;
  if (auto *Invoke = dyn_cast<InvokeInst>(From->getTerminator()))
    return Invoke->getNormalDest() == To;
  // A terminating deoptimization side exit remains a control-flow barrier
  // after polls are removed, so it does not expose speculation of both arms.
  if (auto *Branch = dyn_cast<BranchInst>(From->getTerminator()))
    if (Branch->isConditional()) {
      unsigned OtherIndex = Branch->getSuccessor(0) == To ? 1 : 0;
      BasicBlock *Other = Branch->getSuccessor(OtherIndex);
      return Other->getTerminatingDeoptimizeCall() != nullptr;
    }
  return From->getSingleSuccessor() == To;
}

bool collapseRedundantPolls(Function &F, LoopInfo &LI, DominatorTree &DT,
                            bool NonLoopOnly) {
  jeandle::SafepointWorkBudget Work;
  DenseMap<BasicBlock *, bool> CallsAtExit;
  SmallPtrSet<CallInst *, 16> CallCovered;
  ReversePostOrderTraversal<Function *> RPOT(&F);
  // Only calls, which are never deleted, generate forward coverage. Using
  // candidate polls in both directions could make their proofs depend on one
  // another and delete every poll on an acyclic path.
  for (BasicBlock *BB : RPOT) {
    if (!Work.spend(BB->size() + pred_size(BB) + 1))
      return false;
    bool Covered = !pred_empty(BB) &&
                   llvm::all_of(predecessors(BB), [&](BasicBlock *Pred) {
                     return canPropagateCoverage(Pred, BB, LI, DT) &&
                            CallsAtExit.lookup(Pred);
                   });
    for (Instruction &I : *BB) {
      if (jeandle::isGuaranteedSafepointCall(I)) {
        Covered = true;
      } else if (isSafepointPoll(I)) {
        if (Covered)
          CallCovered.insert(cast<CallInst>(&I));
      } else if (!isGuaranteedToTransferExecutionToSuccessor(&I)) {
        Covered = false;
      }
    }
    CallsAtExit[BB] = Covered;
  }

  DenseMap<BasicBlock *, bool> SafepointsAtEntry;
  SmallVector<CallInst *, 16> Redundant;
  // Postorder visits successors before predecessors, except at cut backedges.
  // A deleted poll depends only on later retained cuts or immutable calls;
  // therefore the complete deletion plan cannot contain a dependency cycle.
  for (BasicBlock *BB : post_order(&F)) {
    if (!Work.spend(BB->size() + succ_size(BB) + 1))
      return false;
    bool Covered =
        !succ_empty(BB) && llvm::all_of(successors(BB), [&](BasicBlock *Succ) {
          return canPropagateCoverage(BB, Succ, LI, DT) &&
                 SafepointsAtEntry.lookup(Succ);
        });
    bool Eligible = !NonLoopOnly || !LI.getLoopFor(BB);
    for (Instruction &I : llvm::reverse(*BB)) {
      if (isSafepointPoll(I)) {
        auto *Poll = cast<CallInst>(&I);
        // The marked outer poll also locates the inner loop's bound proof.
        if (Eligible && !jeandle::isStripMinedPoll(I) &&
            (Covered || CallCovered.contains(Poll)))
          Redundant.push_back(Poll);
        else
          Covered = true;
      } else if (jeandle::isGuaranteedSafepointCall(I)) {
        Covered = true;
      } else if (!isGuaranteedToTransferExecutionToSuccessor(&I)) {
        Covered = false;
      }
    }
    SafepointsAtEntry[BB] = Covered;
  }

  // Budget exhaustion above leaves the entire source poll set untouched.
  for (CallInst *Poll : Redundant) {
    LLVM_DEBUG(dbgs() << "  collapse: erased a redundant poll in "
                      << Poll->getParent()->getName() << "\n");
    Poll->eraseFromParent();
  }
  return !Redundant.empty();
}

// Apply only after the entire loop tree has a complete retention plan. Each
// poll belongs to exactly one loop, including polls selected by ancestors.
static bool deleteLoopPolls(Loop &L, LoopInfo &LI,
                            const RequiredPolls &Retained,
                            SafepointPollEliminationMode Mode) {
  bool Changed = false;
  [[maybe_unused]] unsigned Total = 0, Kept = 0;
  for (BasicBlock *BB : L.blocks()) {
    if (LI.getLoopFor(BB) != &L)
      continue;
    for (Instruction &I : make_early_inc_range(*BB)) {
      if (!isSafepointPoll(I))
        continue;
      ++Total;
      // The outer poll also identifies the strip-mined inner bound proof.
      if (Retained.contains(cast<CallInst>(&I)) ||
          jeandle::isStripMinedPoll(I)) {
        ++Kept;
        continue;
      }
      I.eraseFromParent();
      Changed = true;
    }
  }
  LLVM_DEBUG(if (Total) dbgs()
             << "  poll-elimination<" << modeName(Mode) << "> loop "
             << L.getHeader()->getName() << ": retained-poll-cut (kept " << Kept
             << " of " << Total << ")\n");
  return Changed;
}

struct EmptyLoopExitValueCheck {
  PHINode *Phi;
  Value *OriginalValue;
  const SCEV *ExitValue;
};

struct EmptyLoopPollRemovalPlan {
  Loop *L;
  BasicBlock *Preheader;
  BasicBlock *ExitingBlock;
  BasicBlock *ExitBlock;
  SmallVector<CallInst *, 2> Polls;
  SmallVector<EmptyLoopExitValueCheck, 2> ExitValues;
  // Exit phis whose incoming value is computed in the loop but is used only by
  // dead LCSSA phi chains (or nothing). No state needs preserving for them;
  // their incoming value is poisoned on apply so deleting the loop leaves no
  // dangling use.
  SmallVector<PHINode *, 2> DeadExitPhis;

  /// Pre-condition re-check before applying: this plan still matches the
  /// IR (the loop nest has the same preheader, exiting, exit, LCSSA,
  /// dedicated-exit shape; the recorded polls are still inside the nest and
  /// alive; each exit value's LCSSA phi still carries its original incoming
  /// value). Sibling-plan application can invalidate a queued plan, so each
  /// plan validates itself before it applies (C2's two-phase analyze-then-
  /// mutate pattern, guarded here against interference).
  bool stillStructurallyValid(LoopInfo &LI, DominatorTree &DT) const;
};

bool hasOnlyDeoptStateUses(Value *V, Loop &L, SmallPtrSetImpl<Value *> &Visited,
                           bool &ReachesSafepoint) {
  if (!Visited.insert(V).second)
    return true;
  for (Use &U : V->uses()) {
    auto *I = dyn_cast<Instruction>(U.getUser());
    if (!I)
      return false;
    if (jeandle::isSafepoint(*I)) {
      auto *CB = dyn_cast<CallBase>(I);
      if (!CB || !CB->isBundleOperand(&U) ||
          !CB->isOperandBundleOfType(LLVMContext::OB_deopt, U.getOperandNo()))
        return false;
      ReachesSafepoint = true;
      continue;
    }
    if (I->isDebugOrPseudoInst())
      continue;
    auto *Phi = dyn_cast<PHINode>(I);
    if (!Phi || L.contains(Phi) ||
        !hasOnlyDeoptStateUses(Phi, L, Visited, ReachesSafepoint))
      return false;
  }
  return true;
}

// A loop is empty for this transform only when it contains pure control/IV
// computation plus direct Jeandle polls. Memory accesses and other calls stay
// out of scope even when LLVM could otherwise prove them dead.
std::optional<EmptyLoopPollRemovalPlan>
buildEmptyLoopPollRemovalPlan(Loop *L, LoopInfo &LI, DominatorTree &DT,
                              ScalarEvolution &SE,
                              jeandle::SafepointLoopAnalysis &A) {
  if (!A.checkLoop(*L) || !L->isLCSSAForm(DT) || !L->hasDedicatedExits())
    return std::nullopt;

  BasicBlock *Preheader = L->getLoopPreheader();
  BasicBlock *ExitingBlock = L->getExitingBlock();
  BasicBlock *ExitBlock = L->getUniqueExitBlock();
  if (!Preheader || !ExitingBlock || !ExitBlock || ExitBlock->isEHPad() ||
      ExitBlock->getSinglePredecessor() != ExitingBlock)
    return std::nullopt;

  auto *PreheaderBr = dyn_cast<BranchInst>(Preheader->getTerminator());
  if (!PreheaderBr || !PreheaderBr->isUnconditional() ||
      PreheaderBr->getSuccessor(0) != L->getHeader())
    return std::nullopt;

  // Every loop in the region being removed must be known to terminate. This
  // permits deleting an empty loop nest as one transaction, including the
  // descendant poll on which an enclosing loop's coverage depends.
  for (Loop *Nested : L->getLoopsInPreorder())
    if (isa<SCEVCouldNotCompute>(SE.getConstantMaxBackedgeTakenCount(Nested)))
      return std::nullopt;

  SmallVector<CallInst *, 2> Polls;
  for (BasicBlock *BB : L->blocks()) {
    for (Instruction &I : *BB) {
      if (isSafepointPoll(I)) {
        Polls.push_back(cast<CallInst>(&I));
        continue;
      }
      if (isa<CallBase>(I) || I.mayReadOrWriteMemory() ||
          I.mayHaveSideEffects())
        return std::nullopt;
    }
  }
  if (Polls.empty())
    return std::nullopt;
  // Only ancestors outside the deleted nest can require one of its polls. An
  // ancestor inside the nest disappears in the same transaction.
  jeandle::AncestorPollRequirements AncestorRequirements =
      jeandle::computeAncestorPollRequirements(*L, A);
  for (CallInst *P : Polls)
    if (AncestorRequirements.isRequired(*P, DT))
      return std::nullopt;

  SCEVExpander Rewriter(SE, "jeandle-empty-loop-check");
  SmallVector<EmptyLoopExitValueCheck, 2> ExitValues;
  SmallVector<PHINode *, 2> DeadExitPhis;
  // Keep this narrow: reconstruct values that survive solely for safepoint
  // deopt state, not general reductions with observable live-outs.
  for (PHINode &Phi : ExitBlock->phis()) {
    if (Phi.getNumIncomingValues() != 1 ||
        Phi.getIncomingBlock(0) != ExitingBlock)
      return std::nullopt;

    Value *Incoming = Phi.getIncomingValue(0);
    auto *IncomingInst = dyn_cast<Instruction>(Incoming);
    if (!IncomingInst || !L->contains(IncomingInst))
      continue;

    SmallPtrSet<Value *, 8> Visited;
    bool ReachesSafepoint = false;
    if (!hasOnlyDeoptStateUses(&Phi, *L, Visited, ReachesSafepoint))
      return std::nullopt;
    if (!ReachesSafepoint) {
      // The phi is dead state (unused, or feeds only dead LCSSA phi chains),
      // not a live-out. It must not block deletion; poison its in-loop
      // incoming on apply so no dangling use remains.
      DeadExitPhis.push_back(&Phi);
      continue;
    }
    if (!SE.isSCEVable(Phi.getType()))
      return std::nullopt;

    const SCEV *ExitValue = SE.getSCEVAtScope(IncomingInst, L->getParentLoop());
    if (isa<SCEVCouldNotCompute>(ExitValue) ||
        !SE.isLoopInvariant(ExitValue, L) ||
        !Rewriter.isSafeToExpandAt(ExitValue, Preheader->getTerminator()))
      return std::nullopt;
    ExitValues.push_back({&Phi, Incoming, ExitValue});
  }
  return EmptyLoopPollRemovalPlan{L,
                                  Preheader,
                                  ExitingBlock,
                                  ExitBlock,
                                  std::move(Polls),
                                  std::move(ExitValues),
                                  std::move(DeadExitPhis)};
}

bool EmptyLoopPollRemovalPlan::stillStructurallyValid(LoopInfo &LI,
                                                      DominatorTree &DT) const {
  if (LI.getLoopFor(L->getHeader()) != L ||
      L->getLoopPreheader() != Preheader ||
      L->getExitingBlock() != ExitingBlock ||
      L->getUniqueExitBlock() != ExitBlock || !L->isLCSSAForm(DT) ||
      !L->hasDedicatedExits() ||
      ExitBlock->getSinglePredecessor() != ExitingBlock)
    return false;
  for (CallInst *P : Polls)
    if (!P->getParent() || !L->contains(P) || !isSafepointPoll(*P))
      return false;
  for (const EmptyLoopExitValueCheck &ExitValue : ExitValues)
    if (ExitValue.Phi->getNumIncomingValues() != 1 ||
        ExitValue.Phi->getIncomingBlock(0) != ExitingBlock ||
        ExitValue.Phi->getIncomingValue(0) != ExitValue.OriginalValue)
      return false;
  for (PHINode *Phi : DeadExitPhis)
    if (Phi->getNumIncomingValues() != 1 ||
        Phi->getIncomingBlock(0) != ExitingBlock)
      return false;
  return true;
}

void applyEmptyLoopPollRemovalPlan(const EmptyLoopPollRemovalPlan &Plan,
                                   LoopInfo &LI, DominatorTree &DT,
                                   ScalarEvolution &SE) {
  SCEVExpander Rewriter(SE, "jeandle-empty-loop-delete");
  for (const EmptyLoopExitValueCheck &ExitValue : Plan.ExitValues) {
    Value *Replacement =
        Rewriter.expandCodeFor(ExitValue.ExitValue, ExitValue.Phi->getType(),
                               Plan.Preheader->getTerminator());
    ExitValue.Phi->setIncomingValue(0, Replacement);
  }
  for (PHINode *Phi : Plan.DeadExitPhis)
    Phi->setIncomingValue(0, PoisonValue::get(Phi->getType()));

  LLVM_DEBUG(dbgs() << "  loop-deletion-prep: deleting "
                    << Plan.L->getHeader()->getName() << " and "
                    << Plan.Polls.size() << " poll(s)\n");
  for (CallInst *P : Plan.Polls)
    P->eraseFromParent();
  deleteDeadLoop(Plan.L, &DT, &SE, &LI);
}

static bool completeLoopPollDeletion(LoopInfo &LI, DominatorTree &DT,
                                     ScalarEvolution &SE,
                                     SafepointPollEliminationMode Mode,
                                     bool DeferEmptyLoopDeletion) {
  jeandle::SafepointWorkBudget Work;
  jeandle::SafepointLoopAnalysis A(LI, DT, SE, Work);
  SmallVector<Loop *, 8> Loops(LI.getLoopsInPreorder());
  SmallPtrSet<Loop *, 8> DeferredEmptyLoops;
  if (DeferEmptyLoopDeletion) {
    for (Loop *L : Loops) {
      if (DeferredEmptyLoops.contains(L))
        continue;
      if (!buildEmptyLoopPollRemovalPlan(L, LI, DT, SE, A))
        continue;
      for (Loop *Nested : L->getLoopsInPreorder())
        DeferredEmptyLoops.insert(Nested);
    }
  }

  RequiredPolls Retained;
  // Empty-loop deletion is a separate atomic transaction. Until it succeeds,
  // keep its polls available to the ancestor coverage plan.
  for (Loop *L : DeferredEmptyLoops)
    for (BasicBlock *BB : L->blocks()) {
      if (LI.getLoopFor(BB) != L)
        continue;
      if (!Work.spend(BB->size()))
        return false;
      for (Instruction &I : *BB)
        if (isSafepointPoll(I))
          Retained.insert(cast<CallInst>(&I));
    }
  for (Loop *L : llvm::reverse(Loops))
    if (!DeferredEmptyLoops.contains(L) && !A.retainPolls(*L, Retained))
      return false;
  if (Work.exhausted())
    return false;

  bool Changed = false;
  for (Loop *L : llvm::reverse(Loops)) {
    if (!DeferredEmptyLoops.contains(L))
      Changed |= deleteLoopPolls(*L, LI, Retained, Mode);
  }
  return Changed;
}

} // namespace

bool jeandle::SafepointLoopAnalysis::isEmptyLoopCandidate(Loop &L) {
  auto [It, Inserted] = EmptyCache.try_emplace(&L);
  if (Inserted)
    It->second =
        buildEmptyLoopPollRemovalPlan(&L, LI, DT, SE, *this).has_value();
  return It->second;
}

void SafepointPollElimination::printPipeline(
    raw_ostream &OS, function_ref<StringRef(StringRef)> MapClassName2PassName) {
  static_cast<PassInfoMixin<SafepointPollElimination> *>(this)->printPipeline(
      OS, MapClassName2PassName);
  OS << '<';
  switch (Mode) {
  case SafepointPollEliminationMode::Early:
    OS << "early";
    break;
  case SafepointPollEliminationMode::AfterStripMining:
    OS << "after-strip-mining";
    break;
  case SafepointPollEliminationMode::LoopDeletionPrep:
    OS << "loop-deletion-prep";
    break;
  }
  if (DeferEmptyLoopDeletion)
    OS << ";defer-empty-loop-deletion";
  OS << '>';
}

PreservedAnalyses SafepointPollElimination::run(Function &F,
                                                FunctionAnalysisManager &AM) {
  if (!jeandle::isSafepointEliminationEnabled() ||
      !jeandle::isRootJavaMethodFunction(F))
    return PreservedAnalyses::all();

  LLVM_DEBUG(dbgs() << "poll-elimination<" << modeName(Mode) << "> running on "
                    << F.getName() << "\n");

  auto &LI = AM.getResult<LoopAnalysis>(F);
  ReversePostOrderTraversal<const Function *> RPOT(&F);
  bool HasIrreducibleCFG = containsIrreducibleCFG<const BasicBlock *>(RPOT, LI);
  if (HasIrreducibleCFG) {
    LLVM_DEBUG(dbgs() << "poll-elimination<" << modeName(Mode)
                      << ">: irreducible CFG in " << F.getName()
                      << ", all polls preserved\n");
    return PreservedAnalyses::all();
  }

  if (Mode == SafepointPollEliminationMode::LoopDeletionPrep) {
    auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
    auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
    // Deleting one empty region can expose another empty region in its parent.
    // Iterate to a fixpoint; each application strictly removes a loop nest.
    jeandle::SafepointWorkBudget Work;
    bool Changed = false;
    bool LocalChanged = true;
    while (LocalChanged) {
      LocalChanged = false;
      jeandle::SafepointLoopAnalysis A(LI, DT, SE, Work);
      SmallVector<Loop *, 8> Loops(LI.getLoopsInPreorder());
      SmallVector<EmptyLoopPollRemovalPlan, 4> Plans;
      SmallPtrSet<Loop *, 8> PlannedLoops;
      for (Loop *L : Loops) {
        if (PlannedLoops.contains(L))
          continue;
        if (auto Plan = buildEmptyLoopPollRemovalPlan(L, LI, DT, SE, A)) {
          for (Loop *Nested : L->getLoopsInPreorder())
            PlannedLoops.insert(Nested);
          Plans.push_back(std::move(*Plan));
        }
      }

      if (Work.exhausted())
        break;
      for (EmptyLoopPollRemovalPlan &Plan : Plans) {
        if (!Plan.stillStructurallyValid(LI, DT))
          continue;
        applyEmptyLoopPollRemovalPlan(Plan, LI, DT, SE);
        Changed = LocalChanged = true;
        // Ancestor coverage may depend on a sibling's polls. Rebuild the
        // analysis and plans after each committed deletion before consuming
        // another proof made against the previous CFG.
        break;
      }
    }
    Changed |= collapseRedundantPolls(F, LI, DT, false);
    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }

  if (Mode == SafepointPollEliminationMode::AfterStripMining) {
    auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
    auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
    bool Changed =
        completeLoopPollDeletion(LI, DT, SE, Mode, DeferEmptyLoopDeletion);
    Changed |= collapseRedundantPolls(F, LI, DT, false);
    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }

  assert(Mode == SafepointPollEliminationMode::Early);
  bool Changed = false;
  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
  if (!jeandle::isStripMiningEnabled()) {
    auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
    Changed |=
        completeLoopPollDeletion(LI, DT, SE, Mode, DeferEmptyLoopDeletion);
  }

  Changed |= collapseRedundantPolls(F, LI, DT, jeandle::isStripMiningEnabled());
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
