//===- JavaOpLengthFolding.h - Fold arraylength of new_array -------*- C++
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
// Folds jeandle.arraylength(new_array(..., length, ...)) to the new_array
// length argument, looking through casts, zero-offset GEPs, PHIs, and
// selects. Runs in the pre-PEA high-tier cluster so the folded (possibly
// constant) length feeds loop canonicalization and full unrolling, which in
// turn turns variable-index array accesses into constant-offset ones that
// PEA can virtualize.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_JEANDLE_JAVAOPLENGTHFOLDING_H
#define LLVM_TRANSFORMS_JEANDLE_JAVAOPLENGTHFOLDING_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class JavaOpLengthFolding : public PassInfoMixin<JavaOpLengthFolding> {
public:
  JavaOpLengthFolding() = default;
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_JEANDLE_JAVAOPLENGTHFOLDING_H
