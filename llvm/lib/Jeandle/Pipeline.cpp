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
#include "llvm/IR/PassManager.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Jeandle/CHADevirtualization.h"
#include "llvm/Transforms/Jeandle/ConstantFieldFolding.h"
#include "llvm/Transforms/Jeandle/ExpandNarrowOopCast.h"
#include "llvm/Transforms/Jeandle/InsertGCBarriers.h"
#include "llvm/Transforms/Jeandle/JavaOperationDeletion.h"
#include "llvm/Transforms/Jeandle/JavaOperationLower.h"
#include "llvm/Transforms/Jeandle/JeandleInliner.h"
#include "llvm/Transforms/Jeandle/JeandleNarrowOopMarker.h"
#include "llvm/Transforms/Jeandle/RepeatedConstantFolding.h"
#include "llvm/Transforms/Jeandle/SafepointCoverageVerifier.h"
#include "llvm/Transforms/Jeandle/SafepointElimination.h"
#include "llvm/Transforms/Jeandle/TLSPointerRewrite.h"
#include "llvm/Transforms/Jeandle/TypeCheckElimination.h"
#include "llvm/Transforms/Scalar/ADCE.h"
#include "llvm/Transforms/Scalar/EarlyCSE.h"
#include "llvm/Transforms/Scalar/InstSimplifyPass.h"
#include "llvm/Transforms/Scalar/LICM.h"
#include "llvm/Transforms/Scalar/LoopRotation.h"
#include "llvm/Transforms/Scalar/RewriteStatepointsForGC.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Utils/LCSSA.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"

namespace llvm::jeandle {

Pipeline::Pipeline(OptimizationLevel level, LLVMContext &Ctx,
                   PipelineOptions Options)
    : SI(Ctx, /*DebugLogging=*/false) {
  SI.registerCallbacks(PIC, &MAM);

  PassBuilder PB(nullptr, PipelineTuningOptions(), std::nullopt, &PIC);

  // Register all the basic analyses with the managers.
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  PM = buildJeandlePipeline(PB, level, Options);
}

// TODO: The pass selection/ordering is not optimal. We need to improve it.
ModulePassManager Pipeline::buildJeandlePipeline(PassBuilder &PB,
                                                 OptimizationLevel level,
                                                 PipelineOptions Options) {
  ModulePassManager PM;
  PM.addPass(JavaOperationLower(0));
  FunctionPassManager PreCHACleanup;
  PreCHACleanup.addPass(InstSimplifyPass());
  PreCHACleanup.addPass(TypeCheckElimination());
  PreCHACleanup.addPass(RepeatedConstantFolding());
  PreCHACleanup.addPass(EarlyCSEPass());
  PreCHACleanup.addPass(InstCombinePass());
  PreCHACleanup.addPass(SimplifyCFGPass());
  PreCHACleanup.addPass(ADCEPass());
  PM.addPass(createModuleToFunctionPassAdaptor(std::move(PreCHACleanup)));
  PM.addPass(createModuleToFunctionPassAdaptor(CHADevirtualization()));
  // JeandleInlineDriver owns the inline-specific loop. Devirtualization
  // refinement between inline rounds should be wired inside the driver so
  // inline-scope state can be preserved across IR rewrites.
  switch (Options.Inlining) {
  case InlineMode::Disabled:
    break;
  case InlineMode::Default:
    PM.addPass(JeandleInlineDriver());
    break;
  case InlineMode::AccessorOnly:
    PM.addPass(JeandleInlineDriver(/*InlineAccessorsOnly=*/true));
    break;
  }
  PM.addPass(createModuleToFunctionPassAdaptor(InstSimplifyPass()));
  PM.addPass(createModuleToFunctionPassAdaptor(TypeCheckElimination()));
  PM.addPass(createModuleToFunctionPassAdaptor(RepeatedConstantFolding()));
  PM.addPass(createModuleToFunctionPassAdaptor(TypeCheckElimination()));
  // Strip mining needs SCEV to see each loop's trip count, which means hoisting
  // jeandle.arraylength out of the loop (EarlyCSE) and collapsing the
  // frontend's lcmp/iflt chain into a single icmp (InstCombine). Only pay for
  // these when strip mining is enabled, so the default build is unchanged.
  if (isStripMiningEnabled()) {
    PM.addPass(createModuleToFunctionPassAdaptor(EarlyCSEPass()));
    PM.addPass(createModuleToFunctionPassAdaptor(InstCombinePass()));
    // Put real frontend loops in LoopSimplify + LCSSA form before Early and
    // the canonicalization below. Strip mining restores this form after those
    // passes because they can invalidate it.
    PM.addPass(createModuleToFunctionPassAdaptor(LoopSimplifyPass()));
    PM.addPass(createModuleToFunctionPassAdaptor(LCSSAPass()));
  }
  // First remove polls already proven redundant. Strip mining runs as a
  // separate invocation so it sees a freshly rebuilt MemorySSA.
  PM.addPass(createModuleToFunctionPassAdaptor(
      SafepointElimination(SafepointEliminationMode::Early)));
  if (getSafepointCoverageCheck() != SafepointCoverageCheck::Off)
    PM.addPass(createModuleToFunctionPassAdaptor(SafepointCoverageVerifier()));

  if (isStripMiningEnabled()) {
    // Expose mandatory array-length exits with the smallest canonicalization
    // sequence required by strip mining.
    PM.addPass(createModuleToFunctionPassAdaptor(SimplifyCFGPass()));
    PM.addPass(createModuleToFunctionPassAdaptor(LoopSimplifyPass()));
    PM.addPass(createModuleToFunctionPassAdaptor(LCSSAPass()));
    LoopPassManager LPM;
    LPM.addPass(LoopRotatePass(true, false));
    LPM.addPass(LICMPass(LICMOptions()));
    PM.addPass(createModuleToFunctionPassAdaptor(
        createFunctionToLoopPassAdaptor(std::move(LPM), true)));
    if (isInclusiveLoopVersioningEnabled())
      PM.addPass(createModuleToFunctionPassAdaptor(SafepointElimination(
          SafepointEliminationMode::InclusiveLoopVersioning)));
    PM.addPass(createModuleToFunctionPassAdaptor(
        SafepointElimination(SafepointEliminationMode::StripMining)));
    if (getSafepointCoverageCheck() != SafepointCoverageCheck::Off)
      PM.addPass(
          createModuleToFunctionPassAdaptor(SafepointCoverageVerifier()));
  }
  // TODO: InsertGCBarriers currently inserts high-level barrier calls before
  // O3 because it cannot handle O3 generated memory intrinsics and vector
  // instructions. But the uninlined barrier calls can still block useful
  // optimizations.
  PM.addPass(createModuleToFunctionPassAdaptor(InsertGCBarriers()));

  // Clean up redundant polls exposed by the transformed loop shape before
  // phase-1 lowering consumes jeandle.safepoint_poll calls.
  PM.addPass(createModuleToFunctionPassAdaptor(
      SafepointElimination(SafepointEliminationMode::Cleanup)));
  if (getSafepointCoverageCheck() != SafepointCoverageCheck::Off)
    PM.addPass(createModuleToFunctionPassAdaptor(SafepointCoverageVerifier()));

  if (level == OptimizationLevel::O3) {
    // Re-form LCSSA independently of strip mining, then atomically delete
    // finite empty loops and the polls that prevent their deletion.
    PM.addPass(createModuleToFunctionPassAdaptor(LCSSAPass()));
    PM.addPass(createModuleToFunctionPassAdaptor(
        SafepointElimination(SafepointEliminationMode::LoopDeletionPrep)));
  }

  PM.addPass(JavaOperationLower(1));
  PM.addPass(std::move(PB.buildPerModuleDefaultPipeline(level)));
  PM.addPass(ExpandNarrowOopCast());
  PM.addPass(RewriteStatepointsForGC());
  PM.addPass(createModuleToFunctionPassAdaptor(JeandleNarrowOopMarker()));
  // Phase 9 is reserved for JavaOps that must be lowered after O3/RS4GC.
  //
  // JavaOperationLower(9) lowers GC barriers only after O3/RS4GC. Lowered G1
  // barriers may compute raw addresses derived from oops, such as card-table
  // addresses. Those raw derived addresses are not oops, are not tracked by
  // RS4GC, and the JVM has no mechanism to update them if a safepoint moves
  // the source oop.
  //
  // The required invariant is that the def-use range of each such raw derived
  // address must not contain a safepoint. Lowering barriers too early exposes
  // the raw address computations to O3, which may extend or reuse them across
  // safepoints and break that invariant. Delaying only the lowering preserves
  // optimization opportunities while keeping raw derived addresses local to the
  // final barrier code.
  PM.addPass(JavaOperationLower(9));
  // Erase JavaOp definitions that have been fully lowered (user-empty) and are
  // no longer referenced. This must run after the inline driver and all
  // JavaOperationLower phases: every JavaOp must stay alive while replayed
  // callee bodies may still resolve them by name during inlining.
  PM.addPass(JavaOperationDeletion());
  PM.addPass(createModuleToFunctionPassAdaptor(TLSPointerRewrite()));
  PM.addPass(createModuleToFunctionPassAdaptor(InstSimplifyPass()));
  return PM;
}

void Pipeline::run(Module &M) { PM.run(M, MAM); }

} // end namespace llvm::jeandle
