//===- SafepointCoverageVerifier.cpp - Safepoint Coverage Check -----------===//
//
// Copyright (c) 2026, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// Verifies the invariant SafepointElimination must preserve: every loop
/// either contains a poll whose block dominates the latch (some poll runs on
/// every complete iteration) or has a SCEV-provable trip count within the
/// chunk budget. A violation means a thread inside the loop may not reach a
/// safepoint in bounded time.
///
/// The check is sufficient, not necessary: loops without a unique latch are
/// skipped, and a dominating poll inside a sub-loop counts (every enclosing
/// iteration passes through it). False positives must be fixed by whitelisting
/// the legal shape here, not by weakening the transform.
///
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Jeandle/SafepointCoverageVerifier.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Jeandle/Attributes.h"
#include "llvm/IR/Jeandle/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Jeandle/SafepointElimination.h"

using namespace llvm;

#define DEBUG_TYPE "safepoint-elimination"

static cl::opt<bool> PrintOnly(
    "jeandle-safepoint-coverage-print-only", cl::init(false),
    cl::desc("Report safepoint coverage violations without aborting."));

static bool isLoopCovered(Loop &L, DominatorTree &DT, ScalarEvolution &SE) {
  BasicBlock *Latch = L.getLoopLatch();
  if (!Latch)
    return true; // multi-latch shapes are out of scope for the check

  for (BasicBlock *BB : L.blocks())
    if (DT.dominates(BB, Latch))
      for (Instruction &I : *BB)
        if (jeandle::isSafepointPoll(I))
          return true;

  const auto *MaxC =
      dyn_cast<SCEVConstant>(SE.getConstantMaxBackedgeTakenCount(&L));
  return MaxC && MaxC->getAPInt().ule(jeandle::getSafepointChunkIters());
}

PreservedAnalyses SafepointCoverageVerifier::run(Function &F,
                                                 FunctionAnalysisManager &AM) {
  if (!F.getParent()->getNamedMetadata(
          jeandle::Metadata::JavaMethodCompilation))
    return PreservedAnalyses::all();

  // Template runtime helpers (lower-phase attribute) have their own bounded
  // loops that legitimately run without polls; they are not Java method bodies
  // and the coverage invariant does not apply to them. SafepointElimination
  // skips them for the same reason.
  if (F.hasFnAttribute(jeandle::Attribute::LowerPhase))
    return PreservedAnalyses::all();

  auto &LI = AM.getResult<LoopAnalysis>(F);

  // SafepointElimination skips functions with irreducible cycles (LoopInfo
  // can't see those cycles, so neither it nor this check can reason about
  // their coverage). Match that: certifying only the natural loops here would
  // be a false assurance, so report the function as unverifiable instead.
  ReversePostOrderTraversal<const Function *> RPOT(&F);
  if (containsIrreducibleCFG<const BasicBlock *>(RPOT, LI)) {
    errs() << "SafepointCoverageVerifier: function '" << F.getName()
           << "' has an irreducible CFG; coverage not verified\n";
    return PreservedAnalyses::all();
  }

  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
  auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);

  bool Broken = false;
  for (Loop *L : LI.getLoopsInPreorder()) {
    if (isLoopCovered(*L, DT, SE)) {
      LLVM_DEBUG(dbgs() << "  covered: loop " << L->getHeader()->getName()
                        << " in " << F.getName() << "\n");
      continue;
    }
    Broken = true;
    errs() << "SafepointCoverageVerifier: loop with header '"
           << L->getHeader()->getName() << "' in function '" << F.getName()
           << "' has no dominating safepoint poll and no provable trip "
              "bound\n";
  }

  if (Broken && !PrintOnly)
    report_fatal_error("Jeandle safepoint coverage verification failed");
  return PreservedAnalyses::all();
}
