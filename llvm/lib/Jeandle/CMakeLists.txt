# Jeandle
add_llvm_component_library(LLVMJeandle
  Jeandle.cpp
  Pipeline.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Jeandle

  LINK_COMPONENTS
  Analysis
  JeandleTransforms
  Core
  Passes
  Support
  )
