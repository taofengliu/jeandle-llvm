//===- PartialEscapeIterativeTest.cpp - PEA fixpoint tests ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Jeandle/PartialEscapeIterative.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/SourceMgr.h"
#include "gtest/gtest.h"

using namespace llvm;

namespace {

TEST(PartialEscapeIterativeTest, ReportsCanonicalizationOnlyMutation) {
  LLVMContext Context;
  SMDiagnostic Error;
  std::unique_ptr<Module> M = parseAssemblyString(R"(
    define i32 @test(i32 %x) {
    entry:
      %dead = add i32 %x, 1
      ret i32 0
    }

    !java-method-compilation = !{}
  )",
                                                  Error, Context);
  ASSERT_TRUE(M);
  Function &F = *M->getFunction("test");
  const unsigned Before = F.getInstructionCount();

  PassBuilder PB;
  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  PreservedAnalyses PA = PartialEscapeIterative().run(F, FAM);

  EXPECT_LT(F.getInstructionCount(), Before);
  EXPECT_FALSE(PA.areAllPreserved());
}

} // namespace
