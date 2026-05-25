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
#include "llvm/Transforms/Jeandle/InsertGCBarriers.h"
#include "llvm/Transforms/Jeandle/JavaOperationLower.h"
#include "llvm/Transforms/Jeandle/PartialEscapeAnalysis.h"
#include "llvm/Transforms/Jeandle/PartialEscapeIterative.h"
#include "llvm/Transforms/Jeandle/PartialEscapeTransform.h"
#include "llvm/Transforms/Jeandle/TLSPointerRewrite.h"
#include "llvm/Transforms/Jeandle/TypeCheckElimination.h"
#include "llvm/Transforms/Scalar/InstSimplifyPass.h"
#include "llvm/Transforms/Scalar/RewriteStatepointsForGC.h"
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

ModulePassManager Pipeline::buildJeandlePipeline(PassBuilder &PB,
                                                 OptimizationLevel level) {
  ModulePassManager PM;
  // Canonicalise loops before PEA so processLoop sees a unique
  // preheader/single-backedge for every natural loop. LoopSimplify cannot
  // recover irreducible or indirectbr-entered loops; PEA defends against
  // those cases in processLoop's no-preheader fallback.
  PM.addPass(createModuleToFunctionPassAdaptor(LoopSimplifyPass()));
  // Outer fixpoint. PartialEscapeIterative wraps the
  // analyze+transform pair in a bounded loop with InstCombine/SimplifyCFG/
  // ADCE between rounds. The iteration cap is controlled by
  // `-jeandle-pea-iterations=N` (default 2, matching Graal's
  // EscapeAnalysisIterations). LoopSimplify still runs only once at the
  // entry — the inner SimplifyCFG can dismantle preheaders, but PEA itself
  // is robust to losing them (processLoop's no-preheader fallback handles
  // it), and LoopSimplify is expensive enough that we don't want it per
  // iteration.
  //
  // Pipeline position decision.
  //   Graal runs PEA at exactly ONE position in its hosted tier pipeline —
  //   `FinalPartialEscapePhase` in HighTier (HighTier.java:110), after
  //   inlining/loop opts/GVN, before lowering. The "Final" prefix refers
  //   to the FINAL_PARTIAL_ESCAPE stage flag (a one-shot guard); there is
  //   NO separate early PEA. EscapeAnalysisIterations=2 inside that one
  //   slot is how Graal achieves the iterative re-fold pattern that D1
  //   already implements here.
  //
  //   Jeandle's single slot is positioned BEFORE JavaOperationLower(0),
  //   InstSimplify, TypeCheckElimination, and the standard O2 pipeline.
  //   This is earlier than Graal's HighTier position, but every downstream
  //   pass either (a) leaves the named alloc intrinsics
  //   `jeandle.new_instance` / `jeandle.newarray` untouched (both carry
  //   `"lower-phase"="1"`, while JavaOperationLower(0) only inlines
  //   phase-0 helpers like load_klass/instanceof/arraylength/idiv), or
  //   (b) preserves addrspace(1) pointer types. The intrinsics survive
  //   until JavaOperationLower(1) below, and addrspace(1) survives until
  //   RewriteStatepointsForGC rewrites it to gc-managed pointers — both
  //   of which run AFTER any plausible second PEA slot.
  //
  //   Considered and rejected: a second `PartialEscapeIterative` after the
  //   O2 pipeline (Graal's apparent `FinalPartialEscapePhase` analogue).
  //   The named intrinsics and addrspace(1) survive through O2 + GC
  //   barriers, so it would be technically feasible. However:
  //     - Graal's "Final" PEA is not a second PEA — it is the only PEA.
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
  //       rounds, which is the same canonicalization Graal applies inside
  //       its single PEA slot.
  //   Bumping the iteration default to 2 captures the Graal-equivalent
  //   benefit with one slot.
  PM.addPass(createModuleToFunctionPassAdaptor(PartialEscapeIterative()));
  PM.addPass(JavaOperationLower(0));
  PM.addPass(createModuleToFunctionPassAdaptor(InstSimplifyPass()));
  PM.addPass(createModuleToFunctionPassAdaptor(TypeCheckElimination()));
  PM.addPass(std::move(PB.buildPerModuleDefaultPipeline(level)));
  PM.addPass(createModuleToFunctionPassAdaptor(InsertGCBarriers()));
  PM.addPass(JavaOperationLower(1));
  PM.addPass(createModuleToFunctionPassAdaptor(TLSPointerRewrite()));
  PM.addPass(RewriteStatepointsForGC());
  return PM;
}

void Pipeline::run(Module &M) { PM.run(M, MAM); }

} // end namespace llvm::jeandle
