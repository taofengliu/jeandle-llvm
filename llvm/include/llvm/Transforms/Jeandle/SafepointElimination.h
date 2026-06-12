//===- SafepointElimination.h - Safepoint Elimination ----------*- C++ -*-===//
//
// Copyright (c) 2026, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SAFEPOINT_ELIMINATION_H
#define LLVM_SAFEPOINT_ELIMINATION_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class Instruction;

/// Removes redundant `jeandle.safepoint_poll` calls. Polls are created by the
/// frontend only — each carries the deopt JVM state of its bci, which no LLVM
/// pass can synthesize — so this pass deletes or relocates polls but never
/// creates one. It is a function pass that walks loops itself, because some
/// of the planned cleanups act outside loop scopes.
class SafepointElimination : public PassInfoMixin<SafepointElimination> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

namespace jeandle {
/// Matches a direct call to `jeandle.safepoint_poll`.
bool isSafepointPoll(const Instruction &I);

/// Iteration budget shared by the trip-count-based poll deletion and the
/// safepoint coverage verifier (-jeandle-safepoint-chunk-iters).
uint64_t getSafepointChunkIters();
} // namespace jeandle

} // namespace llvm

#endif // LLVM_SAFEPOINT_ELIMINATION_H
