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
/// Current transform:
///   - Adjacent poll collapse: two polls back to back on a straight-line path
///     collapse to the later one, whose deopt state supersedes the earlier
///     one's at that program point. Mirrors C2's SafePointNode::Identity and
///     Graal's SafepointNode.simplify.
///
/// A poll tagged `!jeandle.poll_coverage` is one some transform designated as
/// its loop's required coverage; it wins over the positional rule.
///
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Jeandle/SafepointElimination.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Jeandle/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

static cl::opt<bool> EnableSafepointElim(
    "jeandle-enable-safepoint-elim", cl::init(true),
    cl::desc("Master switch for the SafepointElimination pass. Setting this "
             "to false makes the pass a no-op. Useful for A/B comparison."));

namespace {

constexpr StringRef SafepointPollName = "jeandle.safepoint_poll";

// A poll carrying this metadata is the designated safepoint coverage of its
// loop: the loop must retain at least one poll so tagged. The tag marks the
// loop's coverage need, not the instruction's identity — clones inherit it
// and any one of them may serve.
constexpr StringRef PollCoverageMD = "jeandle.poll_coverage";

bool isSafepointPoll(const Instruction &I) {
  const auto *CI = dyn_cast<CallInst>(&I);
  if (!CI || CI->isIndirectCall())
    return false;
  const Function *Callee = CI->getCalledFunction();
  return Callee && Callee->getName() == SafepointPollName;
}

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

  bool Changed = false;
  for (BasicBlock &BB : F)
    Changed |= collapseAdjacentPolls(BB);

  if (!Changed)
    return PreservedAnalyses::all();

  // Only calls were erased; the CFG is intact.
  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}
