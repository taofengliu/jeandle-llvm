//===-- PartialEscapeAnalysis.h - PEA (analysis pass) ----------*- C++ -*-===//
//
// Part of the Jeandle JIT compiler.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_JEANDLE_PARTIALESCAPEANALYSIS_H
#define LLVM_TRANSFORMS_JEANDLE_PARTIALESCAPEANALYSIS_H

#include "llvm/IR/Jeandle/PartialEscape.h"
#include "llvm/IR/PassManager.h"

namespace llvm {

class PartialEscapeAnalysis : public AnalysisInfoMixin<PartialEscapeAnalysis> {
  friend AnalysisInfoMixin<PartialEscapeAnalysis>;
  static AnalysisKey Key;

public:
  using Result = jeandle::PEAResult;
  Result run(Function &F, FunctionAnalysisManager &FAM);
};

} // namespace llvm

#endif
