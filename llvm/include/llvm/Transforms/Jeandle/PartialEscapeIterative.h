//===- PartialEscapeIterative.h - PEA outer fixpoint -------------*- C++ -*-===//
//
// Copyright (c) 2026, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Outer fixpoint a la Graal's EffectsPhase.runAnalysis. Runs the
// PartialEscapeAnalysis + PartialEscapeTransform pair in a bounded loop,
// interleaving InstCombine + SimplifyCFG + ADCE between rounds so that
// canonicalization can expose new scalar-replacement opportunities for the
// next round. Mirrors Graal's PartialEscapePhase(iterative=true) ->
// EffectsPhase(maxIterations = EscapeAnalysisIterations).
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

#endif // LLVM_TRANSFORMS_JEANDLE_PARTIALESCAPEITERATIVE_H
