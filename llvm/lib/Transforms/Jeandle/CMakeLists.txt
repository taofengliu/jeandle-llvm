add_llvm_component_library(LLVMJeandleTransforms
  InsertGCBarriers.cpp
  JavaOperationLower.cpp
  PartialEscape.cpp
  PartialEscapeAnalysis.cpp
  PartialEscapeIterative.cpp
  PartialEscapeTransform.cpp
  PartialEscapeUtils.cpp
  TLSPointerRewrite.cpp
  TypeCheckElimination.cpp
  VMConstants.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms/Jeandle

  LINK_COMPONENTS
  Analysis
  Core
  InstCombine
  Scalar
  TransformUtils
  )
