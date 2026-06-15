//===- SafepointElimination.cpp - Jeandle Safepoint Elimination -----------===//
//
// Copyright (c) 2026, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// Removes redundant `jeandle.safepoint_poll` calls, so that loop
/// optimizations are not blocked by opaque calls and the per-iteration poll
/// overhead is reduced, while every loop stays within the configured
/// time-to-safepoint budget: a poll is only deleted when another poll keeps
/// covering the loop or the loop's trip count is provably small.
///
/// Current transforms:
///   - Adjacent poll collapse: two polls back to back on a straight-line path
///     collapse to the later one, whose deopt state supersedes the earlier
///     one's at that program point. Mirrors C2's SafePointNode::Identity and
///     Graal's SafepointNode.simplify.
///   - Keep-one dedup: when some poll dominates a loop's latch, every
///     iteration passes it, so the loop's other polls are redundant. Mirrors
///     C2's IdealLoopTree::remove_safepoints(keep_one=true).
///   - Short-loop deletion: an innermost loop whose trip count provably fits
///     the chunk budget cannot keep a thread away from a safepoint for more
///     than that many iterations, so its polls go entirely. Mirrors C2's
///     short-loop collapse in OuterStripMinedLoopNode::adjust_strip_mined_loop.
///
/// A poll tagged `!jeandle.poll_coverage` is one some transform designated as
/// its loop's required coverage; it wins over the positional rule.
///
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Jeandle/SafepointElimination.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Jeandle/Attributes.h"
#include "llvm/IR/Jeandle/Metadata.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

#define DEBUG_TYPE "safepoint-elimination"

static cl::opt<bool> EnableSafepointElim(
    "jeandle-enable-safepoint-elim", cl::init(true),
    cl::desc("Master switch for the SafepointElimination pass. Setting this "
             "to false makes the pass a no-op. Useful for A/B comparison."));

// One knob, three meanings: the bare-deletion trip bound, the (future)
// strip-mining chunk size, and therefore the system-wide bound on how many
// iterations a thread can run between safepoint polls. C2's equivalent is
// LoopStripMiningIter (default 1000).
static cl::opt<uint64_t> SafepointChunkIters(
    "jeandle-safepoint-chunk-iters", cl::init(1000),
    cl::desc("Iteration budget between safepoint polls: polls are deleted "
             "outright only in innermost loops whose SCEV max backedge-taken "
             "count provably fits this bound."));

uint64_t llvm::jeandle::getSafepointChunkIters() { return SafepointChunkIters; }

bool llvm::jeandle::isSafepointPoll(const Instruction &I) {
  const auto *CI = dyn_cast<CallInst>(&I);
  if (!CI || CI->isIndirectCall())
    return false;
  const Function *Callee = CI->getCalledFunction();
  return Callee && Callee->getName() == "jeandle.safepoint_poll";
}

namespace {

using jeandle::isSafepointPoll;

// A poll carrying this metadata is the designated safepoint coverage of its
// loop: the loop must retain at least one poll so tagged. The tag marks the
// loop's coverage need, not the instruction's identity — clones inherit it
// and any one of them may serve.
constexpr StringRef PollCoverageMD = "jeandle.poll_coverage";

bool hasCoverageMarker(const Instruction &I) {
  return I.getMetadata(PollCoverageMD) != nullptr;
}

// Debug/pseudo-probe and lifetime markers carry no observable side effect, so
// they don't break the back-to-back adjacency of two polls.
bool isAdjacencyTransparent(const Instruction &I) {
  return I.isDebugOrPseudoInst() || I.isLifetimeStartOrEnd();
}

// Collapse runs of adjacent polls within a block, keeping the last one: the
// earlier poll's deopt state belongs to an upstream program point that the
// later poll's state supersedes (the converse — reusing an earlier state for
// a later point — would be wrong). A coverage-marked poll survives over an
// unmarked one regardless of position.
bool collapseAdjacentPolls(BasicBlock &BB) {
  bool Changed = false;
  CallInst *Prev = nullptr;
  for (Instruction &I : llvm::make_early_inc_range(BB)) {
    if (isSafepointPoll(I)) {
      auto *Cur = cast<CallInst>(&I);
      if (Prev) {
        CallInst *Victim =
            (hasCoverageMarker(*Prev) && !hasCoverageMarker(*Cur)) ? Cur : Prev;
        CallInst *Survivor = Victim == Prev ? Cur : Prev;
        LLVM_DEBUG(dbgs() << "  collapse: erased an adjacent poll in "
                          << BB.getName() << "\n");
        Victim->eraseFromParent();
        Changed = true;
        Prev = Survivor;
        continue;
      }
      Prev = Cur;
      continue;
    }
    if (!isAdjacencyTransparent(I))
      Prev = nullptr;
  }
  return Changed;
}

// Keep-one dedup, C2's remove_safepoints(keep_one=true). When some poll the
// loop owns dominates the latch, every complete iteration passes it, so the
// loop's other polls are redundant: keep the latch-closest dominating poll (a
// coverage-marked one wins) and erase the rest. Without a dominating poll,
// deleting any poll could leave an iteration path uncovered — delete nothing.
//
// Only polls in blocks the loop owns directly are considered: an inner loop's
// polls are that loop's coverage and must not be counted on (or deleted) here,
// because they may legitimately disappear later under the inner loop's own
// trip-count proof.
bool keepOneLoopPoll(Loop &L, LoopInfo &LI, DominatorTree &DT) {
  BasicBlock *Latch = L.getLoopLatch();
  if (!Latch)
    return false;

  SmallVector<CallInst *, 4> Polls;
  for (BasicBlock *BB : L.blocks()) {
    if (LI.getLoopFor(BB) != &L)
      continue;
    for (Instruction &I : *BB)
      if (isSafepointPoll(I))
        Polls.push_back(cast<CallInst>(&I));
  }
  if (Polls.size() < 2)
    return false;

  // Walk the dominator chain upward from the latch; every block on it
  // dominates the latch, so the first poll found is the latch-closest
  // dominating one. First pass restricts to coverage-marked polls.
  CallInst *Keep = nullptr;
  for (bool MarkedOnly : {true, false}) {
    for (BasicBlock *BB = Latch; BB && L.contains(BB);) {
      if (LI.getLoopFor(BB) == &L) {
        for (Instruction &I : llvm::reverse(*BB)) {
          if (isSafepointPoll(I) && (!MarkedOnly || hasCoverageMarker(I))) {
            Keep = cast<CallInst>(&I);
            break;
          }
        }
        if (Keep)
          break;
      }
      auto *IDom = DT.getNode(BB)->getIDom();
      BB = IDom ? IDom->getBlock() : nullptr;
    }
    if (Keep)
      break;
  }
  if (!Keep)
    return false;

  LLVM_DEBUG(dbgs() << "  keep-one: loop " << L.getHeader()->getName()
                    << " kept the latch-dominating poll, erased "
                    << (Polls.size() - 1) << " redundant\n");
  for (CallInst *P : Polls)
    if (P != Keep)
      P->eraseFromParent();

  // The survivor is the loop's designated coverage from here on.
  if (!hasCoverageMarker(*Keep))
    Keep->setMetadata(PollCoverageMD, MDNode::get(Keep->getContext(), {}));
  return true;
}

// Trip-count-based deletion. SCEV's constant max backedge-taken count is a
// sound upper bound across every exit and wrap case, so when it fits the
// chunk budget the loop cannot keep a thread away from its next safepoint
// for more than the budget and its polls can go entirely.
//
// Innermost loops only: a short loop enclosing a poll-free short loop would
// compound to budget^2 poll-free iterations and break the bound. Once the
// inner loop dissolves (full unroll, deletion), the enclosing loop becomes
// innermost and qualifies on a later run.
//
// This deletes coverage-marked polls too: a provable trip bound supersedes the
// coverage need (the loop is short enough that no poll is required at all), so
// the marker is not a shield here.
bool deleteShortLoopPolls(Loop &L, ScalarEvolution &SE) {
  if (!L.isInnermost())
    return false;
  const auto *MaxC =
      dyn_cast<SCEVConstant>(SE.getConstantMaxBackedgeTakenCount(&L));
  if (!MaxC || MaxC->getAPInt().ugt(SafepointChunkIters))
    return false;

  SmallVector<CallInst *, 4> Polls;
  for (BasicBlock *BB : L.blocks())
    for (Instruction &I : *BB)
      if (isSafepointPoll(I))
        Polls.push_back(cast<CallInst>(&I));
  if (Polls.empty())
    return false;

  LLVM_DEBUG(dbgs() << "  short-loop: " << L.getHeader()->getName()
                    << " max trip count fits the chunk budget, deleted "
                    << Polls.size() << " poll(s)\n");
  for (CallInst *P : Polls)
    P->eraseFromParent();
  SE.forgetLoop(&L);
  return true;
}

} // end anonymous namespace

PreservedAnalyses SafepointElimination::run(Function &F,
                                            FunctionAnalysisManager &AM) {
  if (!EnableSafepointElim)
    return PreservedAnalyses::all();

  // Only compiled Java methods carry Jeandle safepoint polls; the module-level
  // named metadata mirrors the existing pattern in InsertGCBarriers.
  if (!F.getParent()->getNamedMetadata(
          jeandle::Metadata::JavaMethodCompilation))
    return PreservedAnalyses::all();

  // The compilation module also holds the template runtime helpers (instanceof
  // slow path, array clear, ...), tagged with the lower-phase attribute. They
  // are runtime stubs with their own bounded loops, not Java method bodies —
  // skip them. This also avoids paying loop analyses on every helper per
  // compile.
  if (F.hasFnAttribute(jeandle::Attribute::LowerPhase))
    return PreservedAnalyses::all();

  LLVM_DEBUG(dbgs() << "SafepointElimination: " << F.getName() << "\n");

  bool Changed = false;
  for (BasicBlock &BB : F)
    Changed |= collapseAdjacentPolls(BB);

  auto &LI = AM.getResult<LoopAnalysis>(F);

  // LoopInfo doesn't model irreducible cycles, so a natural loop's
  // latch-dominating poll or trip-count bound says nothing about a thread
  // spinning inside an irreducible sub-cycle: keep-one could delete that
  // cycle's polls while keeping a poll the cycle never reaches, and short-loop
  // deletion bounds only latch executions, not the cycle's internal spins.
  // Skip the loop transforms for the whole function; the block-local collapse
  // above is safe on any CFG.
  ReversePostOrderTraversal<const Function *> RPOT(&F);
  if (containsIrreducibleCFG<const BasicBlock *>(RPOT, LI)) {
    LLVM_DEBUG(dbgs() << "  irreducible CFG; skipping loop transforms\n");
  } else {
    auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
    auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);

    // Innermost-first, so an inner loop settles its own coverage before any
    // outer-loop decision looks at the nest.
    SmallVector<Loop *, 8> Loops(LI.getLoopsInPreorder());
    for (Loop *L : llvm::reverse(Loops)) {
      Changed |= keepOneLoopPoll(*L, LI, DT);
      Changed |= deleteShortLoopPolls(*L, SE);
    }
  }

  if (!Changed)
    return PreservedAnalyses::all();

  // Only calls were erased; the CFG is intact.
  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}
