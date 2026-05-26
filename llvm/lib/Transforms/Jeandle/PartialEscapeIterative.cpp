//===- PartialEscapeIterative.cpp - PEA outer fixpoint --------------------===//
//
// Copyright (c) 2026, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Outer fixpoint. Wraps the existing PartialEscapeAnalysis +
// PartialEscapeTransform pair in a bounded loop that interleaves the standard
// canonicalization passes (InstCombine + SimplifyCFG + ADCE) between rounds.
//
// Why iterate?  Each round of PEA may materialize a virtual at an escape
// point. After DCE removes the now-dead branch that forced materialization,
// the next round can re-virtualize the freshly emitted `jeandle.new_instance`
// invoke (its field stores are still in IR and are picked up naturally by
// tier1Allocate's RPO walk). We get this re-fold for free as long as we
// re-run analysis on the canonicalized IR (see the re-foldable
// materialization note below).
//
// Convergence detection
//   Round N converges when ALL of:
//     - the PartialEscapeTransform pass returned PreservedAnalyses::all()
//       (i.e. PEAResult::hasOptimizationOpportunity() was false), AND
//     - the count of remaining `jeandle.new_instance` / `jeandle.newarray`
//       callsites is identical to round N-1's count, AND
//     - the analyzer's VirtualizationDelta and AllocationDelta are
//       both unchanged from round N-1 (catches transform iterations that
//       fuse multiple CommitAllocations into one — alloc count stable but
//       virtualisation delta still positive), AND
//     - round (N-1)'s canonicalisation did not mutate IR (no
//       `postTriggered` signal — i.e. the previous iter's canonicalisation
//       did not move IR around enough that this iter's analyser should
//       re-check before we accept convergence).
//   The first condition is the analyzer's own "I have nothing to do"
//   signal; the rest guard against transforms that touch IR without
//   eliminating an allocation (e.g. ReplaceLoad on a still-live virtual)
//   or iterations whose post-canonicalisation IR has not yet been re-
//   analysed.
//
// Determinism
//   Each round constructs a fresh PEAResult (FAM.invalidate clears the cached
//   one). SeqNo / ObjectID counters inside that PEAResult naturally reset to
//   zero at construction. Any unparented PHIs / coercion Insts from the
//   previous round are reaped by the previous PEAResult's destructor when
//   FAM evicts it. There is no state carried across rounds other than the
//   IR itself.
//
// Backwards compatibility
//   The cl::opt `-jeandle-pea-iterations=N` defaults to 1, so callers that
//   request only the existing single-round semantics see no behavioural
//   change. Existing lit tests that use
//     `passes="require<partial-escape-analysis>,partial-escape-transform"`
//   bypass this wrapper entirely.
//
// Re-foldable materialization
//   This wrapper does NOT attach metadata to
//   materialized invokes. Instead we rely on the natural path: when
//   applyMaterialize emits a new `jeandle.new_instance` invoke followed by
//   separate `store` instructions for each tracked field, the next round's
//   tier1Allocate recognises that invoke and the RPO walk processes the
//   stores naturally, re-virtualizing the alloc if no escape remains.
//   Lit test 283_outer_fixpoint_b8_natural_refold.ll verifies this end-to-end.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Jeandle/PartialEscapeIterative.h"

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
#include "llvm/Analysis/Jeandle/PartialEscape.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Analysis/Jeandle/PartialEscapeAnalysis.h"
#include "llvm/Transforms/Jeandle/PartialEscapeTransform.h"
#include "llvm/Analysis/Jeandle/PartialEscapeUtils.h"
#include "llvm/Transforms/Scalar/ADCE.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"

#include <limits>

#define DEBUG_TYPE "jeandle-pea-iterative"

using namespace llvm;

// Default iteration cap. We bumped this default from 1 to 2 after
// confirming:
//   1. 177/183 lit tests bypass this wrapper (they invoke
//      `partial-escape-analysis,partial-escape-transform` directly) and so
//      are unaffected by the default.
//   2. The 5 wrapper-using tests pin their own `-jeandle-pea-iterations=N`
//      explicitly (one to test cap-stop at 4, three to validate two-round
//      elimination at 2, one to test convergence at 4). Test 282 — the
//      "default behaviour" guard — was updated in this change.
//   3. The convergence break in `run()` ensures iter 2 is a no-op when
//      iter 1 already reached fixed point, so the cost on simple functions
//      is one extra `countJeandleAllocations` walk + one early exit.
// Tests/benchmarks that need single-round semantics can still opt down with
// `-jeandle-pea-iterations=1`; the recommended hard cap remains 4
// for production but the cl::opt allows up to `kHardIterationCap` (16).
static cl::opt<unsigned> JeandlePEAIterations(
    "jeandle-pea-iterations", cl::init(2), cl::Hidden,
    cl::desc("PEA: maximum number of analyze+transform+canonicalize rounds "
             "in the outer fixpoint. Default 2. Set to 1 for "
             "single-round semantics, 3-4 for aggressive re-fold."));

// PEA-only IR dump hook. When the option is non-empty and
// F.getName() contains the supplied substring, the wrapper dumps F to
// errs() before AND after each PartialEscapeTransform round. Default empty
// (no dump). Only PEA rounds dump, so a `2>&1 | grep PEA-DUMP` filter is
// enough to focus on PEA's IR transitions without polluting the log with
// non-PEA passes. ~10 LOC site at the Iter loop body.
static cl::opt<std::string> JeandleDumpPEAIR(
    "jeandle-dump-pea-ir", cl::init(""), cl::Hidden,
    cl::desc("PEA: dump function IR to errs() before AND after every "
             "PartialEscapeTransform round whose function name contains "
             "the given substring. Empty (the default) disables the dump."));

// Hard upper bound to guard against pathological inputs that produce a
// transform delta every round without ever truly converging. The cl::opt
// can override upward if a benchmark genuinely needs more rounds.
static constexpr unsigned kHardIterationCap = 16;

// Walk every BasicBlock in F and count CallBases that match
// `jeandle.new_instance` or `jeandle.newarray`. We use this as the
// "allocations remaining" signal for convergence detection across rounds.
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
  // PartialEscapeAnalysis::run and PartialEscapeTransform::run so that
  // non-Java functions (template module, runtime stubs) skip both the
  // outer loop and the inner passes entirely.
  Module *M = F.getParent();
  if (!M || !M->getNamedMetadata(jeandle::Metadata::JavaMethodCompilation))
    return PreservedAnalyses::all();

  const unsigned Requested = JeandlePEAIterations;
  if (Requested == 0)
    return PreservedAnalyses::all();
  const unsigned IterCap = std::min(Requested, kHardIterationCap);

  bool AnyChanged = false;
  unsigned PrevAllocs = countJeandleAllocations(F);
  // Track the previous iteration's analyser deltas so we can spot
  // "alloc count steady but virtualisation delta still positive" — a
  // transform that fuses multiple CommitAllocations into one keeps the
  // alloc count stable across iterations while still doing real work.
  // Initialised to a sentinel that mismatches any real first-iter delta so
  // we never break on the first iteration's comparison.
  int PrevVDelta = std::numeric_limits<int>::min();
  int PrevADelta = std::numeric_limits<int>::min();
  // Did the previous iteration's canonicalisation phase change IR?
  // If so, this iteration's analyser must run before we can call convergence;
  // a TransformIdle on iter N alone is not enough when iter (N-1)'s canon
  // moved things around.
  bool PrevCanonChanged = false;

  // Helper: run a sub-pass directly and propagate its PreservedAnalyses
  // back into FAM. The standard FunctionPassManager does this automatically
  // between adjacent passes; here we drive passes one-by-one to interleave
  // convergence checks, so we have to mirror that contract by hand —
  // otherwise the cached analyses (DominatorTree, LoopInfo, ...) consumed
  // by the next sub-pass go stale relative to the freshly mutated IR.
  auto runAndInvalidate = [&](auto &&Pass) {
    PreservedAnalyses Sub = Pass.run(F, FAM);
    FAM.invalidate(F, Sub);
    return Sub;
  };

  // Dump-IR gating. Compute once per run; the substring match
  // never changes mid-fixpoint.
  const bool DumpThisFunc = !JeandleDumpPEAIR.empty() &&
                            F.getName().contains(JeandleDumpPEAIR);

  for (unsigned Iter = 0; Iter < IterCap; ++Iter) {
    if (DumpThisFunc) {
      errs() << ";; PEA-DUMP before iter=" << Iter << " function "
             << F.getName() << "\n"
             << F << "\n";
    }
    // The transform reads PartialEscapeAnalysis via FAM.getResult, which
    // triggers the analyzer if no cached result is present. Each fresh
    // run constructs a brand-new PEAResult — SeqNo and ObjectID counters
    // start at 0, OwnedPhis/OwnedInsts/OwnedLoopFieldPhis are empty, and
    // BlockEffects is empty.
    PartialEscapeTransform Transform;
    // Do NOT use runAndInvalidate here — we need to read the
    // analyser's VirtualizationDelta / AllocationDelta from the cached
    // PEAResult AFTER transform.run() (which built/consumed it) but BEFORE
    // FAM.invalidate drops it. Mirror runAndInvalidate's semantics
    // manually with that intermediate read.
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
    // malformation produced by one round is reported immediately (not after
    // the subsequent InstCombine / SimplifyCFG / ADCE rewrites have mangled
    // the stack trace).
#ifndef NDEBUG
    if (!TransformIdle && verifyFunction(F, &errs())) {
      errs() << "PEA: produced malformed IR for " << F.getName()
             << " at iter=" << Iter << "\n";
      llvm_unreachable("PartialEscapeIterative produced malformed IR");
    }
#endif

    // After the transform we need a fresh count to detect whether any
    // allocations were eliminated this round. eraseAllocation removes
    // the alloc CallBase from the IR, so countJeandleAllocations will
    // observe the drop directly.
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

    // Convergence: break only if the transform
    // did nothing AND the alloc count is stable AND both analyser deltas
    // are stable AND the previous iter's canonicalisation did not mutate
    // IR. Each guard catches a distinct "still doing work" pattern:
    //   * !TransformIdle           — transform mutated IR this iter.
    //   * !AllocsUnchanged         — an allocation appeared/disappeared.
    //   * !VDeltaUnchanged         — virtualisation count moved (e.g.
    //                                CommitAllocation fusion).
    //   * !ADeltaUnchanged         — allocation count moved (defensive
    //                                pair with the IR-level count guard).
    //   * PrevCanonChanged         — previous iter's canonicalisation
    //                                changed something the analyser has
    //                                not yet observed (postTriggered).
    if (TransformIdle && AllocsUnchanged && VDeltaUnchanged && ADeltaUnchanged &&
        !PrevCanonChanged)
      break;

    PrevVDelta = CurVDelta;
    PrevADelta = CurADelta;

    // Canonicalize between rounds — but skip on the last iter (no point
    // since we won't analyze again).
    //
    // Order: DCE → CFG simplification → LoopSimplify → InstCombine.
    // ADCE-first prevents InstCombine from canonicalising against
    // stale-but-dead IR (e.g. a now-dangling materialised alloc whose loads
    // have not been pruned yet, producing a select-fold on a dying value).
    // LoopSimplifyPass re-canonicalises preheaders that SimplifyCFG may
    // have eaten, so a subsequent PEA iteration still sees a single-edge
    // preheader for the loop fixpoint path.
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
      // Re-count after canonicalization in case DCE pruned an alloc whose
      // only uses became dead. Without this, the next iter's convergence
      // check would compare against a stale PrevAllocs.
      PrevAllocs = countJeandleAllocations(F);
    }
    // Roll forward to next iter's "did the prior canon do anything?"
    // probe. Reset-at-start semantics collapses to "the value at the
    // break check reflects the previous iter's canon".
    PrevCanonChanged = CanonChanged;
  }

  return AnyChanged ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
