//===- TypeCheckElimination.h - Eliminate redundant type checks -----------===//
//
// Copyright (c) 2025, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TYPE_CHECK_ELIMINATION_H
#define LLVM_TYPE_CHECK_ELIMINATION_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class TypeCheckElimination : public PassInfoMixin<TypeCheckElimination> {
public:
  TypeCheckElimination() {}
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);
};

} // namespace llvm

#endif // LLVM_TYPE_CHECK_ELIMINATION_H
