//===- PartialEscapeIterative.cpp - PEA outer fixpoint --------------------===//
//
// Copyright (c) 2026, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Outer fixpoint. Re-runs PartialEscapeAnalysis + PartialEscapeTransform,
// interleaving the standard canonicalization passes (ADCE + SimplifyCFG +
// LoopSimplify + InstCombine) between rounds, until the IR is stable. Each
// round may materialize a virtual at an escape point; after DCE removes the
// now-dead branch, the next round can re-virtualize the freshly emitted
// `jeandle.new_instance` invoke because its field stores are still in IR.
//
// Convergence (see `run()`): the transform is idle AND the remaining alloc
// count AND both analyser deltas (VirtualizationDelta, AllocationDelta) are
// stable AND the previous round's canonicalization did not mutate IR. The
// round cap is configured by `-jeandle-pea-iterations` (hard-capped at
// HardIterationCap). Determinism holds because each round builds a fresh
// PEAResult via FAM.invalidate, so no analyser state crosses rounds.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Jeandle/PartialEscapeIterative.h"

#include "llvm/Analysis/Jeandle/PartialEscape.h"
#include "llvm/Analysis/Jeandle/PartialEscapeAnalysis.h"
#include "llvm/Analysis/Jeandle/PartialEscapeUtils.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Jeandle/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Jeandle/PartialEscapeTransform.h"
#include "llvm/Transforms/Scalar/ADCE.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"

#include <limits>

#define DEBUG_TYPE "jeandle-pea-iterative"

using namespace llvm;

// Default 2 rounds. The convergence break in run() makes round 2 a no-op
// when round 1 already reached fixed point; the hard cap is
// HardIterationCap (16). Lit tests that pin -jeandle-pea-iterations=N are
// unaffected.
static cl::opt<unsigned> JeandlePEAIterations(
    "jeandle-pea-iterations", cl::init(2), cl::Hidden,
    cl::desc("PEA: maximum number of analyze+transform+canonicalize rounds "
             "in the outer fixpoint. Default 2. Set to 1 for "
             "single-round semantics, 3-4 for aggressive re-fold."));

// PEA-only IR dump hook. When non-empty and matching F.getName(), dumps F
// to errs() before and after each PartialEscapeTransform round. Filter with
// `2>&1 | grep PEA-DUMP` to isolate PEA IR transitions.
static cl::opt<std::string> JeandleDumpPEAIR(
    "jeandle-dump-pea-ir", cl::init(""), cl::Hidden,
    cl::desc("PEA: dump function IR to errs() before AND after every "
             "PartialEscapeTransform round whose function name contains "
             "the given substring. Empty (the default) disables the dump."));

static cl::list<std::string> JeandleDumpPEAIRFunctions(
    "jeandle-dump-pea-ir-function", cl::Hidden,
    cl::desc("PEA: restrict IR dumps to functions whose name exactly matches "
             "one of the supplied names. May be repeated. Empty (the "
             "default) preserves substring-filter behavior."),
    cl::value_desc("function"));

static bool matchesExactDumpFunction(StringRef FunctionName) {
  if (JeandleDumpPEAIRFunctions.empty())
    return true;
  for (const std::string &Allowed : JeandleDumpPEAIRFunctions)
    if (FunctionName == StringRef(Allowed))
      return true;
  return false;
}

// Hard upper bound guarding against non-converging inputs.
static constexpr unsigned HardIterationCap = 16;

bool llvm::jeandle::isPEAEnabled() { return JeandlePEAIterations != 0; }

// Count `jeandle.new_instance` / `jeandle.new_array` CallBases in F — the
// "allocations remaining" signal for convergence detection.
static unsigned countJeandleAllocations(Function &F) {
  unsigned Count = 0;
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (auto *CB = dyn_cast<CallBase>(&I)) {
        if (jeandle::pea::isJeandleAllocation(CB))
          ++Count;
      }
    }
  }
  return Count;
}

PreservedAnalyses
PartialEscapeIterative::run(Function &F, FunctionAnalysisManager &FAM) {
  // Gate on jeandle.java_method_compilation. Mirrors the gating in
  // PartialEscapeAnalysis::run and PartialEscapeTransform::run so non-Java
  // functions skip the loop entirely.
  Module *M = F.getParent();
  if (!M || !M->getNamedMetadata(jeandle::Metadata::JavaMethodCompilation))
    return PreservedAnalyses::all();

  const unsigned Requested = JeandlePEAIterations;
  if (Requested == 0)
    return PreservedAnalyses::all();
  const unsigned IterCap = std::min(Requested, HardIterationCap);

  bool AnyChanged = false;
  unsigned PrevAllocs = countJeandleAllocations(F);
  // Previous iteration's analyser deltas. A transform that fuses multiple
  // CommitAllocations into one keeps the alloc count stable while still
  // doing work, so the virtualisation delta is tracked too. Sentinels
  // mismatch any real first-iter delta so the first iteration never breaks.
  int PrevVDelta = std::numeric_limits<int>::min();
  int PrevADelta = std::numeric_limits<int>::min();
  // Whether the previous iteration's canonicalization changed IR. If so,
  // the analyser must run again before convergence can be declared.
  bool PrevCanonChanged = false;

  // Run a sub-pass directly and invalidate FAM with its PreservedAnalyses,
  // mirroring what FunctionPassManager does between adjacent passes so the
  // cached analyses (DominatorTree, LoopInfo, ...) stay fresh for the next.
  auto runAndInvalidate = [&](auto &&Pass) {
    PreservedAnalyses Sub = Pass.run(F, FAM);
    FAM.invalidate(F, Sub);
    return Sub;
  };

  // Dump-IR gating (legacy substring and exact allowlist matches are fixed
  // for the run).
  const bool HasDumpFilter =
      !JeandleDumpPEAIR.empty() || !JeandleDumpPEAIRFunctions.empty();
  const bool LegacyDumpMatches =
      JeandleDumpPEAIR.empty() || F.getName().contains(JeandleDumpPEAIR);
  const bool DumpThisFunc = HasDumpFilter && LegacyDumpMatches &&
                            matchesExactDumpFunction(F.getName());

  for (unsigned Iter = 0; Iter < IterCap; ++Iter) {
    if (DumpThisFunc) {
      errs() << ";; PEA-DUMP before iter=" << Iter << " function "
             << F.getName() << "\n"
             << F << "\n";
    }
    // The transform triggers PartialEscapeAnalysis via FAM.getResult, which
    // constructs a fresh PEAResult for this round.
    PartialEscapeTransform Transform;
    // Run by hand (not via runAndInvalidate) so we can read the analyser's
    // deltas from the cached PEAResult before FAM.invalidate drops it.
    PreservedAnalyses TransformPA = Transform.run(F, FAM);
    int CurVDelta = 0;
    int CurADelta = 0;
    if (auto *Cached =
            FAM.getCachedResult<PartialEscapeAnalysis>(F)) {
      CurVDelta = Cached->VirtualizationDelta;
      CurADelta = Cached->AllocationDelta;
    }
    FAM.invalidate(F, TransformPA);
    const bool TransformIdle = TransformPA.areAllPreserved();
    AnyChanged |= !TransformIdle;

    if (DumpThisFunc) {
      errs() << ";; PEA-DUMP after iter=" << Iter << " function "
             << F.getName() << " transform_idle=" << TransformIdle << "\n"
             << F << "\n";
    }

    // In debug builds, validate IR at each iteration boundary so a
    // malformation is reported before later canonicalization rewrites mangle
    // the stack trace.
#ifndef NDEBUG
    if (!TransformIdle && verifyFunction(F, &errs())) {
      errs() << "PEA: produced malformed IR for " << F.getName()
             << " at iter=" << Iter << "\n";
      llvm_unreachable("PartialEscapeIterative produced malformed IR");
    }
#endif

    // Fresh count to detect allocation elimination this round; eraseAllocation
    // removes the alloc CallBase, so the drop is observed directly.
    const unsigned NowAllocs = countJeandleAllocations(F);
    const bool AllocsUnchanged = (NowAllocs == PrevAllocs);
    PrevAllocs = NowAllocs;

    const bool VDeltaUnchanged = (CurVDelta == PrevVDelta);
    const bool ADeltaUnchanged = (CurADelta == PrevADelta);

    LLVM_DEBUG({
      dbgs() << "PartialEscapeIterative[" << F.getName() << "] iter=" << Iter
             << " transform_idle=" << TransformIdle
             << " allocs=" << NowAllocs
             << " v_delta=" << CurVDelta << " a_delta=" << CurADelta
             << " prev_canon_changed=" << PrevCanonChanged << "\n";
    });

    // Convergence: transform idle, alloc count and both analyser deltas
    // stable, and the previous round's canonicalization did not mutate IR.
    if (TransformIdle && AllocsUnchanged && VDeltaUnchanged && ADeltaUnchanged &&
        !PrevCanonChanged)
      break;

    PrevVDelta = CurVDelta;
    PrevADelta = CurADelta;

    // Canonicalize between rounds, skipping the last iter (no further
    // analysis). Order: ADCE → SimplifyCFG → LoopSimplify → InstCombine.
    // ADCE-first prevents InstCombine from folding against stale-but-dead
    // IR; LoopSimplify restores preheaders SimplifyCFG may have removed so
    // the next round still sees a single-edge loop preheader.
    bool CanonChanged = false;
    if (Iter + 1 < IterCap) {
      ADCEPass Dc;
      SimplifyCFGPass Sc;
      LoopSimplifyPass Ls;
      InstCombinePass Ic;
      CanonChanged |= !runAndInvalidate(Dc).areAllPreserved();
      CanonChanged |= !runAndInvalidate(Sc).areAllPreserved();
      CanonChanged |= !runAndInvalidate(Ls).areAllPreserved();
      CanonChanged |= !runAndInvalidate(Ic).areAllPreserved();
      // Re-count in case ADCE pruned an alloc whose only uses became dead.
      PrevAllocs = countJeandleAllocations(F);
    }
    // Carry this round's canonicalization result into the next iteration's
    // convergence check.
    PrevCanonChanged = CanonChanged;
  }

  return AnyChanged ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
