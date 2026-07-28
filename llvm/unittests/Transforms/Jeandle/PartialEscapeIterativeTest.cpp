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
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"

using namespace llvm;

namespace {

static PreservedAnalyses runPartialEscapeIterative(Function &F) {
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
  return PartialEscapeIterative().run(F, FAM);
}

static std::string printFunctionIR(const Function &F) {
  std::string IR;
  raw_string_ostream OS(IR);
  F.print(OS);
  return IR;
}

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

  PreservedAnalyses PA = runPartialEscapeIterative(F);

  EXPECT_LT(F.getInstructionCount(), Before);
  EXPECT_FALSE(PA.areAllPreserved());
}

TEST(PartialEscapeIterativeTest,
     IgnoresIntermediateInvalidationWhenCanonicalIRIsUnchanged) {
  LLVMContext Context;
  SMDiagnostic Error;
  std::unique_ptr<Module> M = parseAssemblyString(R"(
    declare hotspotcc void @jeandle.safepoint_poll()
    declare i32 @__gxx_personality_v0(...)

    define i32 @test(i32 %limit)
        gc "hotspotgc" personality ptr @__gxx_personality_v0 {
    entry:
      %has.iteration = icmp sgt i32 %limit, 0
      br i1 %has.iteration, label %loop.preheader, label %exit

    loop.preheader:
      br label %loop

    exit.loopexit:
      br label %exit

    exit:
      %result = phi i32 [ 0, %entry ], [ %next, %exit.loopexit ]
      call hotspotcc void @jeandle.safepoint_poll()
          [ "deopt"(i32 0, i32 0, i64 99, i32 0, i64 65546,
                    i32 %result) ]
      ret i32 %result

    loop:
      %index = phi i32 [ %next, %loop ], [ 0, %loop.preheader ]
      %next = add nuw nsw i32 %index, 1
      call hotspotcc void @jeandle.safepoint_poll()
          [ "deopt"(i32 0, i32 0, i64 65546, i32 %next) ]
      %done = icmp eq i32 %next, %limit
      br i1 %done, label %exit.loopexit, label %loop
    }

    !java-method-compilation = !{}
  )",
                                                  Error, Context);
  ASSERT_TRUE(M);
  Function &F = *M->getFunction("test");
  runPartialEscapeIterative(F);
  const std::string Before = printFunctionIR(F);

  PreservedAnalyses PA = runPartialEscapeIterative(F);

  EXPECT_EQ(Before, printFunctionIR(F));
  EXPECT_TRUE(PA.areAllPreserved());
}

} // namespace
