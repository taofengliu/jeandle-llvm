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

enum class SafepointEliminationMode {
  Early,
  InclusiveLoopVersioning,
  StripMining,
  Cleanup,
  LoopDeletionPrep,
};

/// Removes redundant `jeandle.safepoint_poll` calls. Polls are created by the
/// frontend only — each carries the deopt JVM state of its bci, which no LLVM
/// pass can synthesize — so this pass deletes or relocates polls but never
/// creates one. It is a function pass that walks loops itself, because some
/// of the planned cleanups act outside loop scopes.
class SafepointElimination : public PassInfoMixin<SafepointElimination> {
public:
  explicit SafepointElimination(
      SafepointEliminationMode Mode = SafepointEliminationMode::Early)
      : Mode(Mode) {}

  void printPipeline(raw_ostream &OS,
                     function_ref<StringRef(StringRef)> MapClassName2PassName) {
    static_cast<PassInfoMixin<SafepointElimination> *>(this)->printPipeline(
        OS, MapClassName2PassName);
    switch (Mode) {
    case SafepointEliminationMode::Early:
      OS << "<early>";
      break;
    case SafepointEliminationMode::InclusiveLoopVersioning:
      OS << "<inclusive-loop-versioning>";
      break;
    case SafepointEliminationMode::StripMining:
      OS << "<strip-mining>";
      break;
    case SafepointEliminationMode::Cleanup:
      OS << "<cleanup>";
      break;
    case SafepointEliminationMode::LoopDeletionPrep:
      OS << "<loop-deletion-prep>";
      break;
    }
  }

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);

private:
  SafepointEliminationMode Mode;
};

namespace jeandle {
/// Matches a direct call to `jeandle.safepoint_poll`.
bool isSafepointPoll(const Instruction &I);

/// Iteration budget shared by the trip-count-based poll deletion and the
/// safepoint coverage verifier (-jeandle-safepoint-chunk-iters).
uint64_t getSafepointChunkIters();

/// Whether strip mining is enabled (-jeandle-enable-strip-mining). The pipeline
/// only runs the EarlyCSE/InstCombine canonicalization that feeds strip mining
/// when this is on, so the default build is unaffected.
bool isStripMiningEnabled();

/// Whether runtime inclusive-loop versioning is enabled
/// (-jeandle-enable-inclusive-loop-versioning).
bool isInclusiveLoopVersioningEnabled();
} // namespace jeandle

} // namespace llvm

#endif // LLVM_SAFEPOINT_ELIMINATION_H
