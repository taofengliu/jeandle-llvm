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
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Jeandle/SafepointElimination.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Jeandle/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

static cl::opt<bool> EnableSafepointElim(
    "jeandle-enable-safepoint-elim", cl::init(true),
    cl::desc("Master switch for the SafepointElimination pass. Setting this "
             "to false makes the pass a no-op. Useful for A/B comparison."));

PreservedAnalyses SafepointElimination::run(Function &F,
                                            FunctionAnalysisManager &AM) {
  if (!EnableSafepointElim)
    return PreservedAnalyses::all();

  // Only compiled Java methods carry Jeandle safepoint polls; the module-level
  // named metadata mirrors the existing pattern in InsertGCBarriers.
  if (!F.getParent()->getNamedMetadata(
          jeandle::Metadata::JavaMethodCompilation))
    return PreservedAnalyses::all();

  return PreservedAnalyses::all();
}
