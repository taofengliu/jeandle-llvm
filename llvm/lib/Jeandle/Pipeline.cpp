//===- Pipeline.cpp - Jeandle Pipeline ------------------------------------===//
//
// Copyright (c) 2025, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Jeandle/Pipeline.h"
#include "llvm/Analysis/Jeandle/PartialEscapeAnalysis.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Jeandle/ConstantFieldFolding.h"
#include "llvm/Transforms/Jeandle/InsertGCBarriers.h"
#include "llvm/Transforms/Jeandle/JavaOperationLower.h"
#include "llvm/Transforms/Jeandle/PartialEscapeIterative.h"
#include "llvm/Transforms/Jeandle/PartialEscapeTransform.h"
#include "llvm/Transforms/Jeandle/RepeatedConstantFolding.h"
#include "llvm/Transforms/Jeandle/TLSPointerRewrite.h"
#include "llvm/Transforms/Jeandle/TypeCheckElimination.h"
#include "llvm/Transforms/Scalar/ADCE.h"
#include "llvm/Transforms/Scalar/InstSimplifyPass.h"
#include "llvm/Transforms/Scalar/RewriteStatepointsForGC.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"

namespace llvm::jeandle {

Pipeline::Pipeline(OptimizationLevel level, LLVMContext &Ctx)
    : SI(Ctx, /*DebugLogging=*/false) {
  SI.registerCallbacks(PIC, &MAM);

  PassBuilder PB(nullptr, PipelineTuningOptions(), std::nullopt, &PIC);

  // Register all the basic analyses with the managers.
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  PM = buildJeandlePipeline(PB, level);
}

// TODO: The pass selection/ordering is not optimal. We need to improve it.
ModulePassManager Pipeline::buildJeandlePipeline(PassBuilder &PB,
                                                 OptimizationLevel level) {
  ModulePassManager PM;
  // Pre-PEA cleanup. PEA's correctness depends on the CFG containing no
  // statically-unreachable edges: a constant-condition branch's dead arm
  // would otherwise feed merges and PHIs with no-op contributions and (for
  // edges into blocks with side-effecting calls) cause spurious
  // materialisations of virtuals on paths that never execute. We delegate
  // this to upstream LLVM: SimplifyCFG folds constant-cond branches and
  // deletes blocks unreachable from entry (via removeUnreachableBlocks);
  // ADCE drops the now-dead instructions that fed the folded conditions;
  // InstCombine exposes any further foldable conditions for the next
  // iteration. LoopSimplify runs after SimplifyCFG so it restores the
  // unique-preheader / single-backedge canonical form that SimplifyCFG
  // may have dismantled — processLoop relies on it for every natural
  // loop, and falls back gracefully for irreducible or indirectbr-entered
  // loops that LoopSimplify cannot recover. This mirrors the
  // inter-iteration canonicalisation that PartialEscapeIterative runs
  // between rounds.
  PM.addPass(createModuleToFunctionPassAdaptor(ADCEPass()));
  PM.addPass(createModuleToFunctionPassAdaptor(SimplifyCFGPass()));
  PM.addPass(createModuleToFunctionPassAdaptor(LoopSimplifyPass()));
  PM.addPass(createModuleToFunctionPassAdaptor(InstCombinePass()));
  // Outer fixpoint. PartialEscapeIterative wraps the
  // analyze+transform pair in a bounded loop with InstCombine/SimplifyCFG/
  // ADCE between rounds. The iteration cap is controlled by
  // `-jeandle-pea-iterations=N` (default 2).
  //
  // Pipeline position decision.
  //   Jeandle runs PEA at exactly ONE position. The slot is BEFORE
  //   JavaOperationLower(0), InstSimplify, TypeCheckElimination, and the
  //   standard O2 pipeline. Every downstream pass either (a) leaves the
  //   named alloc intrinsics `jeandle.new_instance` / `jeandle.new_array`
  //   untouched (both carry `"lower-phase"="1"`, while JavaOperationLower(0)
  //   only inlines phase-0 helpers like load_klass/instanceof/arraylength/
  //   idiv), or (b) preserves addrspace(1) pointer types. The intrinsics
  //   survive until JavaOperationLower(1) below, and addrspace(1) survives
  //   until RewriteStatepointsForGC rewrites it to gc-managed pointers.
  //
  //   Considered and rejected: a second `PartialEscapeIterative` after the
  //   O2 pipeline. The named intrinsics and addrspace(1) survive through
  //   O2 + GC barriers, so it would be technically feasible. However:
  //     - Phase-0 helpers (load_klass / instanceof / arraylength /
  //       div/rem) do not allocate or escape; inlining them via
  //       JavaOperationLower(0) cannot expose new allocation-virtualization
  //       opportunities for a second PEA round to capture.
  //     - O2's stock passes (InstCombine, SimplifyCFG, GVN, SROA, LICM,
  //       loop unroll) cannot SROA or fold addrspace(1) loads — they have
  //       no Java semantic model for the heap — so they neither destroy
  //       PEA invariants nor expose meaningful new escape decisions.
  //     - Intra-PEA fixpoint already iterates through any re-foldable
  //       materializations exposed by InstCombine+SimplifyCFG+ADCE between
  //       rounds.
  PM.addPass(createModuleToFunctionPassAdaptor(PartialEscapeIterative()));
  PM.addPass(JavaOperationLower(0));
  PM.addPass(createModuleToFunctionPassAdaptor(InstSimplifyPass()));
  PM.addPass(createModuleToFunctionPassAdaptor(TypeCheckElimination()));
  PM.addPass(createModuleToFunctionPassAdaptor(RepeatedConstantFolding()));
  PM.addPass(createModuleToFunctionPassAdaptor(TypeCheckElimination()));
  PM.addPass(createModuleToFunctionPassAdaptor(InsertGCBarriers()));
  PM.addPass(JavaOperationLower(1));
  PM.addPass(createModuleToFunctionPassAdaptor(TLSPointerRewrite()));
  PM.addPass(std::move(PB.buildPerModuleDefaultPipeline(level)));
  PM.addPass(RewriteStatepointsForGC());
  return PM;
}

void Pipeline::run(Module &M) { PM.run(M, MAM); }

} // end namespace llvm::jeandle
