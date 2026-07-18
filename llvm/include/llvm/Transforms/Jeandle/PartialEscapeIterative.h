//===- PartialEscapeIterative.h - PEA outer fixpoint -------------*- C++
//-*-===//
//
// Copyright (c) 2026, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Outer fixpoint. Runs PartialEscapeAnalysis + PartialEscapeTransform in a
// bounded loop, interleaving canonicalization passes (ADCE + SimplifyCFG +
// LoopSimplify + InstCombine) between rounds so new scalar-replacement
// opportunities are exposed for the next round.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_JEANDLE_PARTIALESCAPEITERATIVE_H
#define LLVM_TRANSFORMS_JEANDLE_PARTIALESCAPEITERATIVE_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class PartialEscapeIterative
    : public PassInfoMixin<PartialEscapeIterative> {
public:
  PartialEscapeIterative() = default;
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);
};

} // namespace llvm

namespace llvm::jeandle {

// True when the PEA fixpoint will do work, i.e. -jeandle-pea-iterations > 0.
// Used by the Jeandle pipeline to gate the whole PEA segment (pre-PEA
// high-tier cluster, pre-PEA cleanup, PEA itself, and post-PEA cleanup) so
// that disabling PEA removes all of it from the pipeline.
bool isPEAEnabled();

} // namespace llvm::jeandle

#endif // LLVM_TRANSFORMS_JEANDLE_PARTIALESCAPEITERATIVE_H
