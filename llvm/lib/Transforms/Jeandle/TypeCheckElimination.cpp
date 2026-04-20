//===- TypeCheckElimination.cpp - Eliminate redundant type checks ---------===//
//
// Copyright (c) 2025, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass eliminates redundant jeandle.check_instanceof calls by using
// compile-time Java type information. It replaces calls with constant true
// (when the object's type is provably a subtype) or constant false (when the
// object's exact type is provably not a subtype).
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Jeandle/TypeCheckElimination.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Jeandle/JavaType.h"
#include "llvm/IR/Jeandle/Metadata.h"
#include "llvm/IR/Jeandle/VMCallback.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "type-check-elimination"

using namespace llvm;

PreservedAnalyses TypeCheckElimination::run(Function &F,
                                            FunctionAnalysisManager &FAM) {
  Module *M = F.getParent();
  if (!M->getNamedMetadata(jeandle::Metadata::JavaMethodCompilation))
    return PreservedAnalyses::all();

  const jeandle::VMCallbacks *CB = jeandle::getVMCallbacks();
  assert(CB && CB->IsSubtype && CB->IsInterface && "VMCallbacks must be set");

  Function *CheckFn = M->getFunction("jeandle.check_instanceof");
  if (!CheckFn)
    return PreservedAnalyses::all();

  DominatorTree &DT = FAM.getResult<DominatorTreeAnalysis>(F);

  // Collect all check_instanceof calls.
  SmallVector<CallInst *, 16> Checks;
  for (auto &I : instructions(F)) {
    auto *CI = dyn_cast<CallInst>(&I);
    if (CI && CI->getCalledFunction() == CheckFn)
      Checks.push_back(CI);
  }

  bool Changed = false;
  for (CallInst *CI : Checks) {
    uintptr_t SuperKlass = jeandle::extractKlassConstant(CI->getArgOperand(0));
    if (SuperKlass == 0)
      continue;

    Value *Obj = CI->getArgOperand(1);
    jeandle::JavaType ObjType = jeandle::getJavaType(Obj, DT, CI);

    // --- Fold to true: known subtype ---
    if (ObjType.isKnown() && CB->IsSubtype(ObjType.Klass, SuperKlass)) {
      LLVM_DEBUG(dbgs() << "TCE: known subtype, replacing with true: " << *CI
                        << "\n");
      CI->replaceAllUsesWith(ConstantInt::getTrue(CI->getType()));
      CI->eraseFromParent();
      Changed = true;
      continue;
    }

    // --- Fold to false ---
    bool FoldToFalse = false;

    if (ObjType.isKnown() && !CB->IsSubtype(ObjType.Klass, SuperKlass)) {
      if (ObjType.Exact) {
        // Exact type and not a subtype → definitely false.
        LLVM_DEBUG(dbgs() << "TCE: exact type not subtype\n");
        FoldToFalse = true;
      } else if (!CB->IsSubtype(SuperKlass, ObjType.Klass) &&
                 !CB->IsInterface(ObjType.Klass) &&
                 !CB->IsInterface(SuperKlass)) {
        // Neither type is a subtype of the other, and both are classes.
        // Java's single class inheritance → no object can be both.
        LLVM_DEBUG(dbgs() << "TCE: incompatible class types\n");
        FoldToFalse = true;
      }
    }

    // Check negative constraints: if SuperKlass is a subtype of any excluded
    // klass, the object can't be SuperKlass (excluding X implies excluding
    // all subtypes of X).
    if (!FoldToFalse && ObjType.hasExclusions()) {
      for (uintptr_t Excluded : ObjType.ExcludedKlasses) {
        if (CB->IsSubtype(SuperKlass, Excluded)) {
          LLVM_DEBUG(dbgs() << "TCE: denied by excluded klass " << Excluded
                            << "\n");
          FoldToFalse = true;
          break;
        }
      }
    }

    if (FoldToFalse) {
      LLVM_DEBUG(dbgs() << "TCE: replacing with false: " << *CI << "\n");
      CI->replaceAllUsesWith(ConstantInt::getFalse(CI->getType()));
      CI->eraseFromParent();
      Changed = true;
    }
  }

  if (!Changed)
    return PreservedAnalyses::all();

  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  PA.preserve<DominatorTreeAnalysis>();
  return PA;
}
