//===- SafepointBudgetStripMining.h - Bounded poll batches -----*- C++ -*-===//
//
// Copyright (c) 2026, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_JEANDLE_SAFEPOINTBUDGETSTRIPMINING_H
#define LLVM_TRANSFORMS_JEANDLE_SAFEPOINTBUDGETSTRIPMINING_H

#include "llvm/IR/PassManager.h"

namespace llvm {

// Bound each poll-free batch independently of the source induction variables.
class SafepointBudgetStripMining
    : public PassInfoMixin<SafepointBudgetStripMining> {
public:
  explicit SafepointBudgetStripMining(bool DeferEmptyLoopDeletion = false)
      : DeferEmptyLoopDeletion(DeferEmptyLoopDeletion) {}

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);

private:
  bool DeferEmptyLoopDeletion;
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_JEANDLE_SAFEPOINTBUDGETSTRIPMINING_H
