//===-- PartialEscapeTransform.h - PEA (transform pass) --------*- C++ -*-===//
//
// Part of the Jeandle JIT compiler.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_JEANDLE_PARTIALESCAPETRANSFORM_H
#define LLVM_TRANSFORMS_JEANDLE_PARTIALESCAPETRANSFORM_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class PartialEscapeTransform : public PassInfoMixin<PartialEscapeTransform> {
public:
  PartialEscapeTransform() = default;
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);
};

} // namespace llvm

#endif
