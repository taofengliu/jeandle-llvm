add_llvm_component_library(LLVMJeandleTransforms
  InsertGCBarriers.cpp
  JavaOperationLower.cpp
  PartialEscapeIterative.cpp
  PartialEscapeTransform.cpp
  TLSPointerRewrite.cpp
  TypeCheckElimination.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms/Jeandle
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Analysis/Jeandle

  LINK_COMPONENTS
  Analysis
  Core
  InstCombine
  Scalar
  TransformUtils
  )
