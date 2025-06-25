//===- JavaOperationLower.cpp - Lower Java Operations ---------------------===//
//
// Copyright (c) 2025, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Jeandle/JavaOperationLower.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Jeandle/Attributes.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/Cloning.h"

using namespace llvm;
using namespace llvm::jeandle;

#define DEBUG_TYPE "java-operation-lower"

namespace {

static bool runImpl(Module &M, int Phase) {
  bool Changed = false;
  SmallVector<CallBase *, 16> CallInstructions;
  InlineFunctionInfo IFI;
  SmallVector<Function *, 16> FunctionsToRemove;

  for (Function &F : M) {
    if (!F.hasFnAttribute(attr::JavaMethod))
      continue;

    if (F.isDeclaration())
      continue;

    for (Instruction &I : instructions(F)) {
      if (auto *CB = dyn_cast<CallBase>(&I)) {
        // Skip instructions that are not lowered in this phase.
        Function *Callee = CB->getCalledFunction();
        if (!Callee || Callee->isDeclaration())
          continue;

        if (!Callee->hasFnAttribute(attr::LowerPhase))
          continue;

        Attribute LowerPhase = Callee->getFnAttribute(attr::LowerPhase);
        int LowerPhaseValue;
        bool Failed =
            LowerPhase.getValueAsString().getAsInteger(10, LowerPhaseValue);
        assert(!Failed && "wrong value of LowerPhase attribute");
        if (LowerPhaseValue != Phase)
          continue;

        CallInstructions.push_back(CB);
      }
    }

    // Inline
    for (CallBase *CB : CallInstructions) {
      if (InlineFunction(*CB, IFI).isSuccess()) {
        Changed = true;
        LLVM_DEBUG(dbgs() << "Successfully inlined: "
                          << CB->getCalledFunction()->getName()
                          << " in lower phase: " << Phase << "\n");
      } else {
        LLVM_DEBUG(dbgs() << "Failed to inline: "
                          << CB->getCalledFunction()->getName()
                          << " in lower phase: " << Phase << "\n");
      }
    }
  }

  // Remove unused functions.
  for (Function &F : M) {
    if (!F.hasFnAttribute(attr::LowerPhase))
      continue;
    Attribute LowerPhase = F.getFnAttribute(attr::LowerPhase);
    int LowerPhaseValue;
    bool Failed =
        LowerPhase.getValueAsString().getAsInteger(10, LowerPhaseValue);
    assert(!Failed && "wrong value of LowerPhase attribute");
    if (LowerPhaseValue == Phase) {
      assert(!F.hasFnAttribute(attr::JavaMethod) &&
             "inlined function should not be java method");
      FunctionsToRemove.push_back(&F);
    }
  }
  for (Function *F : FunctionsToRemove) {
    LLVM_DEBUG(dbgs() << "Remove unused function: " << F->getName()
                      << " in lower phase: " << Phase << "\n");
    F->eraseFromParent();
    Changed = true;
  }

  return Changed;
}

} // end anonymous namespace

namespace llvm {

PreservedAnalyses JavaOperationLower::run(Module &M,
                                          ModuleAnalysisManager &MAM) {
  if (!runImpl(M, Phase))
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}

} // end namespace llvm
