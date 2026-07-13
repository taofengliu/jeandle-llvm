//===- SafepointCoverageVerifier.h - Safepoint Coverage Check --*- C++ -*-===//
//
// Copyright (c) 2026, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SAFEPOINT_COVERAGE_VERIFIER_H
#define LLVM_SAFEPOINT_COVERAGE_VERIFIER_H

#include "llvm/IR/PassManager.h"

namespace llvm {

/// Checks that every loop in a compiled Java method either has each latch
/// dominated by a safepoint poll or has a provably small trip count — the
/// invariant SafepointElimination must preserve. Like SafepointIRVerifier this
/// checks a sufficient (not necessary) condition and is meant as a diagnostic
/// gate, not part of the main IR verifier.
class SafepointCoverageVerifier
    : public PassInfoMixin<SafepointCoverageVerifier> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

namespace jeandle {
/// Mode of the safepoint coverage verifier
/// (-jeandle-verify-safepoint-coverage): Off does not run it; Warn reports
/// every violation but continues; Fatal aborts on the first violating function.
enum class SafepointCoverageCheck { Off, Warn, Fatal };
SafepointCoverageCheck getSafepointCoverageCheck();
} // namespace jeandle

} // namespace llvm

#endif // LLVM_SAFEPOINT_COVERAGE_VERIFIER_H
