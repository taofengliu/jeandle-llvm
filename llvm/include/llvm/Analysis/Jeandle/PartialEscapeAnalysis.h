//===-- PartialEscapeAnalysis.h - PEA (analysis pass) ----------*- C++ -*-===//
//
// Copyright (c) 2026, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Partial Escape Analysis, the analysis pass. Tracks Java objects allocated
// at jeandle.new_instance / jeandle.new_array sites that have not yet
// escaped, and records every decided IR mutation as a jeandle::Effect in the
// returned jeandle::PEAResult. As required by LLVM's Analysis/Transform
// split, this pass never mutates the IR; PartialEscapeTransform consumes the
// result and applies the effects.
//
//===---------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_JEANDLE_PARTIALESCAPEANALYSIS_H
#define LLVM_ANALYSIS_JEANDLE_PARTIALESCAPEANALYSIS_H

#include "llvm/Analysis/Jeandle/PartialEscape.h"
#include "llvm/IR/PassManager.h"

namespace llvm {

// New-PM function analysis producing jeandle::PEAResult (see PartialEscape.h
// for the data model). The companion transform pass PartialEscapeTransform
// applies the recorded effects.
class PartialEscapeAnalysis : public AnalysisInfoMixin<PartialEscapeAnalysis> {
  friend AnalysisInfoMixin<PartialEscapeAnalysis>;
  static AnalysisKey Key;

public:
  using Result = jeandle::PEAResult;

  Result run(Function &F, FunctionAnalysisManager &FAM);
};

} // namespace llvm

#endif
