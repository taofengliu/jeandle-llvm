set(LLVM_LINK_COMPONENTS
  Analysis
  AsmParser
  Core
  JeandleTransforms
  Passes
  Scalar
  TransformUtils
  )

add_llvm_unittest(JeandleTransformsTests
  PartialEscapeIterativeTest.cpp
  )
