//===- PartialEscapeAnalysis.cpp - PEA (analysis pass) ---------------------===//
//
// Copyright (c) 2026, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Real partial-escape semantics. When a virtual object encounters an
// instruction the analyzer can't fold (an "escape" point — generic call,
// ret, store of the virtual into non-virtual memory, etc.) we record a
// Materialize effect at that point. The transform pass re-emits a real
// allocation immediately before the escape, replays the tracked field
// stores, and RAUWs the original allocation's result with the new one. The
// original allocation invoke is still erased via EliminateAllocation; the
// virtually-folded stores/loads before the escape still apply.
//
// Multi-predecessor merge: every block's per-object exit state (Virtual /
// Materialized, tracked field values, lock counts) is snapshotted into
// BlockExits; at the top of every block we reconstruct the per-block state
// either by inheriting from the unique pred's snapshot or by running
// mergeStates() over all predecessors' snapshots. An object stays virtual
// at BB's entry IFF every predecessor reports it Virtual, all predecessors
// agree on the tracked field values at every offset, AND all predecessors
// agree on the lock count. Any disagreement (mixed virtual / materialized,
// missing on some path, field mismatch, lock mismatch) marks the object
// ineligible — the commit() sweep drops every recorded effect for it and
// the original IR survives unchanged.
//
// PHI handling. processBlockPhis classifies every explicit LLVM PHI of
// a java-heap pointer at a multi-predecessor merge into one of three cases.
// The classification depends on how the PHI's incoming values resolve
// against the per-pred BlockExits virtual sets:
//
//   - Case A (mixed or fallback). At least one incoming is non-virtual,
//     OR a Case C attempt bailed. For every virtual incoming the analyzer
//     materializes the VO at that incoming's predecessor terminator; the
//     PHI itself stays in IR as a real-pointer merge over the materialized
//     allocations and the already-non-virtual incomings.
//
//   - Case B (uniform ID). Every incoming resolves to the SAME ObjectID
//     AND the object is still virtual at merge entry. The PHI is registered
//     as an alias for that ObjectID in AliasMap so downstream load/store
//     handlers fold through it; no new VO and no materialization. If the
//     underlying allocation is later eliminated by Pass 2, the PHI is dead
//     (all incomings RAUW'd to poison) and is erased explicitly so the IR
//     doesn't carry a `phi [poison, ..., poison]` artefact past PEA.
//
//   - Case C (compatible but distinct IDs). Every incoming carries a
//     virtual ObjectID but the IDs DIFFER across incomings. synthesizeCaseC
//     attempts to merge them into a single synthetic VirtualObject (cloned
//     from the first per-pred VO). Compatibility requires identical Klass /
//     kind / entry count / lock state across preds, plus an identity check
//     (the PHI is the only external LLVM user of each per-pred alloc, and
//     no other VO references the per-pred VOs via virtualRef). Per-entry
//     field PHIs are emitted for offsets where the per-pred values
//     disagree; agreeing entries fold to a single value. If the synthetic
//     VO later requires materialization (e.g. a downstream escape consumes
//     the PHI), it is conservatively dropped: both the synthetic VO and
//     every per-pred source VO are marked ineligible so the original IR
//     survives. The cascade-materialize path that keeps the PHI as
//     the materialized pointer is deferred to a future task. On any
//     compatibility / identity / per-entry-type failure the PHI falls
//     through to Case A.
//
// Lock cascade: when materializing a virtual whose LockCount > 0, the
// analyzer (1) drops the previously-recorded ReplaceCall(true) effects
// targeting the unbalanced enter call sites, (2) emits ReplaceInput effects
// retargeting each enter's first operand onto the materialized pointer, and
// (3) clears the live stack. Matching exits downstream of the escape point
// are not elided in the first place (foldMonitorExit on a materialized
// object returns false) and survive in IR. Under
// the runtime's RequiresStrictLockOrder VMCallback (overridable via the
// JeandleAssumeStrictLockOrder cl::opt for tests), every other still-locked
// virtual is also cascaded into materialization at the same insertion point.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/Jeandle/PartialEscapeAnalysis.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Jeandle/JavaType.h"
#include "llvm/IR/Jeandle/Metadata.h"
#include "llvm/Analysis/Jeandle/PartialEscape.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Jeandle/VMCallback.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Analysis/Jeandle/PartialEscapeUtils.h"

#include <unordered_map>

#define DEBUG_TYPE "partial-escape-analysis"

using namespace llvm;

// Per-statistic counters surfaced via LLVM's standard
// `-stats` flag (Statistic.h). Counts are bumped at the analyzer's effect-
// emission sites; a small drift vs. the final committed effect set is
// possible when a late dropEffectsFor() strips a Materialize that was
// already counted (Eligible flipped after emission). Treated as a
// diagnostic, not an audit.
STATISTIC(JeandlePEAVirtualized,
          "Number of virtual objects PEA created");
STATISTIC(JeandlePEAEliminated,
          "Number of allocations eliminated by PEA");
STATISTIC(JeandlePEAMaterialized,
          "Number of materializations emitted by PEA");
STATISTIC(JeandlePEAMaterializedPHI,
          "Materializations triggered by PHI / Case-A merge");
STATISTIC(JeandlePEAMaterializedMerge,
          "Materializations triggered by state merge");
STATISTIC(JeandlePEAMaterializedLoopExit,
          "Materializations at loop preheader (force-drain)");
STATISTIC(JeandlePEAMaterializedUnhandled,
          "Materializations for unhandled instruction (escape point)");
// Extended counters. The first three measure how aggressively
// the analyzer drives its loop fixpoint, and so help calibrate
// kMaxLoopFixpointIters / JeandlePEALoopCutoff. The last two split the
// "Unhandled" bucket so cascade and nested materializations can be
// audited separately from the user-visible escape-point reason.
STATISTIC(JeandlePEALoopFixpointRetries,
          "Sum of inner-fixpoint iterations across every processLoop call");
STATISTIC(JeandlePEAOuterFixpointIterations,
          "Top-level processLoop entries (one per top-level loop traversal)");
STATISTIC(JeandlePEAModeEscalations,
          "Regular -> MaterializeAll mode flips (escalations)");
STATISTIC(JeandlePEAMaterializedCascade,
          "Materializations triggered by strict-lock cascade");
STATISTIC(JeandlePEAMaterializedNested,
          "Materializations triggered by nested-virtual prerequisite");

// When set, dump per-function PEA classification stats
// on errs() after commit(). The line is prefixed with `;; PEA stats` so it
// is greppable both as IR-comment-shaped output and as a regular stderr
// line. Counts are derived from PEAResult::EscapeClassification (populated
// by commit()).
static llvm::cl::opt<bool> JeandleDumpPEAStats(
    "jeandle-dump-pea-stats", llvm::cl::init(false), llvm::cl::Hidden,
    llvm::cl::desc("PEA: dump per-function NeverEscapes / PartiallyEscapes "
                   "/ AlwaysEscapes counts on errs() after analysis."));

// Cascade other still-locked virtuals at materialization when the target
// uses HotSpot's lightweight locking (LM_LIGHTWEIGHT requires strict lock
// nesting on the runtime stack). The effective value is resolved per-run
// by Analyzer::resolveStrictLockOrder(): an explicit command-line setting
// (getNumOccurrences() > 0) wins for testing override, otherwise the
// RequiresStrictLockOrder VMCallback is consulted. Final fallback (no
// callback registered, no override) is true, matching JDK 21+ defaults
// on x86_64.
static llvm::cl::opt<bool> JeandleAssumeStrictLockOrder(
    "jeandle-assume-strict-lock-order", llvm::cl::init(true), llvm::cl::Hidden,
    llvm::cl::desc("PEA: testing override for whether the target VM requires "
                   "strict lock nesting (cascades still-locked virtuals on "
                   "materialization). When unset, the value is queried from "
                   "the RequiresStrictLockOrder VM callback."));

// Cap on array length for virtualization candidates. Promoted to a
// hidden cl::opt so tests can override it without rebuilding; the default
// is 128. A JVM-tunable surface (-XX:JeandleMaxEscapeAnalysisArrayLength=N)
// can map onto this later without churn to the data structures.
static llvm::cl::opt<unsigned> MaximumEscapeAnalysisArrayLength(
    "jeandle-pea-max-array-length", llvm::cl::init(128), llvm::cl::Hidden,
    llvm::cl::desc("PEA: cap on array length eligible for virtualization. "
                   "Larger arrays bypass PEA. Default 128."));

// Testing-only knob — when true, every top-level processLoop entry
// immediately switches to Mode::MaterializeAll for the duration of the
// fixpoint, so the deferred end-of-block Materialize emission path is
// exercised without having to construct a pathological non-converging
// fixpoint in the IR. Default off; lit tests opt in via
// -jeandle-pea-force-materialize-all=true. Not surfaced through any
// VMCallback or runtime knob — purely a debug / test aid.
static llvm::cl::opt<bool> JeandlePEAForceMaterializeAll(
    "jeandle-pea-force-materialize-all", llvm::cl::init(false), llvm::cl::Hidden,
    llvm::cl::desc("PEA: force every top-level processLoop into "
                   "Mode::MaterializeAll on entry. Testing aid for "
                   "virtualize-then-materialise coverage."));

// Loop nesting DEPTH threshold for the 3-state mode machine. When a
// top-level processLoop encounters a nest whose maximum loop depth exceeds
// this value, the analyzer transiently enters Mode::StopNewInLoopNest for
// the duration of that nest: tier1Allocate refuses to register NEW virtual
// allocations inside the nest, but every other operation (already-virtual
// merge / lock / load-fold / store-fold) continues unchanged. This bounds
// the worst-case exponential cost of processing a very deep nest while
// preserving virtualisation for objects allocated outside the nest.
// Default 20.
//
// IMPORTANT: this is the DEPTH cutoff. Distinct from Analyzer::
// kMaxLoopFixpointIters (10), which caps the number of BODY iterations
// within a single processLoop fixpoint.
static llvm::cl::opt<unsigned> JeandlePEALoopCutoff(
    "jeandle-pea-loop-cutoff", llvm::cl::init(20), llvm::cl::Hidden,
    llvm::cl::desc("PEA: loop nesting depth threshold. When a nest's maximum "
                   "depth exceeds this value, the analyzer enters the "
                   "Mode::StopNewInLoopNest state for the duration of the "
                   "nest (refuse new virtualisations but keep tracking "
                   "existing virtuals). Default 20."));

// Filter PEA to functions whose name contains a given substring.
// When the option is non-empty, Analyzer::run() returns an empty PEAResult
// for any function whose name does not contain the filter; the transform
// pass then sees nothing to commit. Empty (the default) means "analyze
// every Java method". Used to focus PEA on a specific compilation, and by
// lit tests that want to exercise gating.
static llvm::cl::opt<std::string> JeandleEscapeAnalyzeOnly(
    "jeandle-pea-analyze-only", llvm::cl::init(""), llvm::cl::Hidden,
    llvm::cl::desc("PEA: only analyze functions whose name contains the "
                   "supplied substring. Empty (the default) analyzes every "
                   "Java method gated by jeandle.java_method_compilation."));

// Per-effect dbgs() trace. The cl::opt itself lives in
// PartialEscape.cpp (the centralised effect-emission site at
// PEAResult::addBlockEffect is the only consumer), so no declaration is
// needed in this translation unit. Turn on with -jeandle-trace-pea for
// lit-time diagnostics.

AnalysisKey PartialEscapeAnalysis::Key;

namespace {

// A live (still-unbalanced) monitorenter on a virtual object.
// The Call is the original jeandle.monitorenter call site (used by
// materializeAt to undo the ReplaceCall elision). Two depth-style fields are
// carried side-by-side:
//   * Order is a per-Analyzer-run, strictly monotonically increasing tag
//     assigned at every push to any LiveLockEnters stack. It is the legacy
//     proxy for monitor lock-depth and is still consulted by
//     blockExitInfoEquivalent comparisons of LiveLockEnters across
//     loop-fixpoint iterations (where it is intentionally IGNORED —
//     the fixpoint convergence check compares Call identity only because
//     Order is monotonically refreshed on every re-push and would otherwise
//     diverge forever).
//   * BytecodeDepth is the Java-bytecode-level monitor depth at the enter
//     site. When the JDK frontend supplies a `!jeandle.lock_depth`
//     i32 metadata node on the call, foldMonitorEnter reads it and stores
//     the value here. When the metadata is absent (lit tests, or a JDK
//     build that predates Phase-1 emission), the field falls back to the
//     Order value at push time — sound for the duration of a single
//     processBlock walk but, like Order, unstable across loop-fixpoint
//     iterations (so the fixpoint check continues to ignore both).
// Cascade decisions (PEA narrow rule, pre-cascade, Phase-2 merge-time
// stack-identity) compare BytecodeDepth when the metadata is supplied and
// otherwise degrade to the existing Order proxy.
struct LockEnter {
  llvm::CallBase *Call;
  uint32_t Order;
  uint32_t BytecodeDepth;
};

// Per-block-exit snapshot of the analyzer's per-object state. We record
// this AFTER processing a block so successors can either inherit directly
// (single live pred) or merge across multiple preds. Keeping the state
// per-block — rather than one global accumulating state — is what makes
// multi-pred merge correct in the presence of branches that mutate the field
// values of a virtual independently.
//
// BlockExitData carries the raw per-object data fields. BlockExitInfo
// inherits from it and adds the exception-edge state-splitting
// book-keeping that lives alongside the normal exit data.
struct BlockExitData {
  DenseSet<jeandle::ObjectID> Virtuals;
  DenseSet<jeandle::ObjectID> Materialized;
  DenseMap<jeandle::ObjectID, DenseMap<int64_t, jeandle::FieldValue>>
      FieldStates;
  DenseMap<jeandle::ObjectID, unsigned> LockCounts;
  // Per-object live monitorenter stack at block exit. Each entry is an
  // unbalanced monitorenter call site (i.e. its matching monitorexit hasn't
  // been seen yet on this path) PLUS a per-run-monotonic order tag (see
  // LockEnter). Sized identically to LockCounts[ID]. Used by materializeAt
  // to undo only the path-relevant elisions; the Order field powers the
  // narrow cascade rule.
  DenseMap<jeandle::ObjectID, SmallVector<LockEnter, 4>> LiveLockEnters;
  // Per-object materialized Value* at block exit. For an object in
  // Materialized, this is the LLVM pointer that downstream merges should use
  // as the PHI input on this predecessor edge. Initially the placeholder is
  // the original allocation (VObj.AllocationCall); the transform redirects
  // through MatPerBlock at apply time.
  DenseMap<jeandle::ObjectID, Value *> MaterializedValues;
};

struct BlockExitInfo : BlockExitData {
  // Exception-edge state-splitting. Only meaningful when the block's
  // terminator is an InvokeInst (which has both a normal and an unwind
  // successor).
  //
  // Jeandle's per-block analyzer processes the terminator-invoke last; the
  // materialize Effects emitted by the invoke are physically inserted
  // immediately before the invoke and survive on BOTH IR successors
  // (definitional dominance). The state-split here is purely about which
  // VirtualMap / FieldStates each successor INHERITS from the analyzer's
  // book-keeping: the normal successor sees the post-call state (the base
  // data of this struct), while the unwind successor sees the pre-call
  // snapshot recorded in UnwindData (the materialize logically happened
  // during the call, so on unwind any partially-materialized state is
  // unobservable to the handler).
  //
  // TerminatorInvoke / UnwindDest are stashed so the analyzer's pred-state
  // lookup (exitDataFor) can detect "this pred's terminator is an invoke
  // whose unwind dest equals the successor block I'm processing".
  llvm::InvokeInst *TerminatorInvoke = nullptr;
  BasicBlock *UnwindDest = nullptr;
  // The invoke was fully virtualized (e.g. tier2JavaOpFold emitted a
  // ReplaceCall effect on it). The transform replaces such invokes with
  // an unconditional branch to the normal dest, so from the analyzer's
  // perspective the unwind edge is dead — the handler is unreachable for
  // analysis purposes. exitDataFor returns nullptr when a successor asks
  // for a killed unwind edge, which makes the merge consumer treat the
  // pred as contributing nothing on that path (mark-as-dead).
  bool UnwindEdgeKilled = false;
  // Pre-invoke snapshot of the per-object data. Captured by processBlock
  // immediately before applying the terminator-invoke, then stashed here
  // only if the post-invoke base data differs (i.e. the invoke actually
  // changed some per-object state, e.g. by materializing an operand).
  // When unset (and UnwindEdgeKilled is false), the unwind successor
  // inherits the same base data as the normal successor (no state-split
  // needed because the invoke was a no-op for PEA).
  std::optional<BlockExitData> UnwindData;
};

// Resolve the effective "strict lock order" value for one analyzer run.
// Precedence:
//   1. If the JeandleAssumeStrictLockOrder cl::opt was explicitly set on the
//      command line (getNumOccurrences() > 0): honor it (testing override).
//   2. Otherwise, if the RequiresStrictLockOrder VMCallback is registered:
//      call it once and use its value (1 == strict, 0 == relaxed).
//   3. Otherwise (no callback, no override): fall back to the cl::opt's
//      default (true). This keeps lit-test behavior unchanged when no JVM
//      is wired up.
//
// The result is cached for the lifetime of an Analyzer instance, so the
// VMCallback fires at most once per compilation — which keeps the callback
// log's sequential-consistency invariant trivially satisfied (it appears
// exactly once at the start of each compilation's recording, and replay
// matches that ordering).
static bool resolveStrictLockOrder() {
  if (JeandleAssumeStrictLockOrder.getNumOccurrences() > 0)
    return JeandleAssumeStrictLockOrder;
  if (const jeandle::VMCallbacks *CB = jeandle::getVMCallbacks()) {
    if (CB->RequiresStrictLockOrder)
      return CB->RequiresStrictLockOrder() != 0;
  }
  return JeandleAssumeStrictLockOrder;
}

class Analyzer {
public:
  Analyzer(Function &F, DominatorTree &DT, LoopInfo &LI)
      : F(F), DT(DT), LI(LI), DL(F.getParent()->getDataLayout()),
        StrictLockOrder(resolveStrictLockOrder()) {}

  jeandle::PEAResult run();

  // Cap on iterations for the loop-fixpoint.
  static constexpr unsigned kMaxLoopFixpointIters = 10;

  // Three states:
  //   Regular            — tier1Allocate registers virtuals as today.
  //   StopNewInLoopNest  — tier1Allocate refuses NEW virtualisations
  //                        inside the active loop nest, but already-virtual
  //                        objects, loads/stores, merges, locks, and exits all
  //                        continue to be tracked exactly as in Regular. Set
  //                        transiently at top-level processLoop entry when
  //                        the nest's max depth exceeds JeandlePEALoopCutoff,
  //                        then restored on convergence/exit.
  //   MaterializeAll     — tier1Allocate registers AND immediately schedules
  //                        an end-of-block materialise for the new VO
  //                        (virtualize-then-materialise), so intra-block folds
  //                        survive. Set transiently by processLoop's overflow
  //                        path when the fixpoint did not converge, then
  //                        reverted on retry.
  enum class Mode : uint8_t { Regular, StopNewInLoopNest, MaterializeAll };

private:
  Function &F;
  DominatorTree &DT;
  LoopInfo &LI;
  const DataLayout &DL;
  // Cached "strict lock order" decision for this run; see
  // resolveStrictLockOrder() for the precedence rules. Replaces the prior
  // direct read of the JeandleAssumeStrictLockOrder cl::opt.
  const bool StrictLockOrder;
  jeandle::PEAResult Result;
  jeandle::AliasMap Aliases;
  // Per-block accumulating object state. Reset at the top of every block from
  // the predecessor snapshots (single inherit / multi-pred merge); rebuilt as
  // the instructions in the block are processed; snapshotted to BlockExits at
  // the end of the block.
  jeandle::PEABlockState CurrentState;

  // Per-object field state: ObjectID -> (offset -> FieldValue). Decoupled from
  // ObjectState::Entries because field discovery is lazy and we don't want to
  // keep VirtualObject::Fields and ObjectState::Entries in lock step.
  DenseMap<jeandle::ObjectID, DenseMap<int64_t, jeandle::FieldValue>>
      FieldStates;

  // Per-object eligibility flag. Function-wide: starts true at allocation;
  // flipped to false on any escape (non-constant offset, type mismatch,
  // nested-virtual store/load, opaque consumer, incompatible multi-pred
  // merge). Once false, commit() drops every recorded effect for the object.
  DenseMap<jeandle::ObjectID, bool> Eligible;

  // Per-object monitor lock counter. Incremented on a folded monitorenter,
  // decremented on a folded monitorexit. Any object with LockCount != 0 at
  // commit time is marked ineligible (unbalanced locking).
  DenseMap<jeandle::ObjectID, unsigned> LockCounts;

  // Per-block live monitorenter stack per ObjectID. Pushed by a folded
  // monitorenter on the receiver, popped by the matching monitorexit. At any
  // point the stack contains exactly the unbalanced enter call sites whose
  // matching exits haven't been seen yet on this path; size == LockCounts[ID].
  // Reset+inherited per block (same lifecycle as LockCounts) so siblings in
  // a diamond CFG don't share stacks. materializeAt walks this stack to
  // decide which ReplaceCall elisions to undo. Each entry also carries a
  // per-run-monotonic Order tag (see LockEnter) serving as the lock-depth
  // stand-in for the narrow cascade rule. back().Order == this VO's max
  // lock depth, front().Order == this VO's min lock depth.
  DenseMap<jeandle::ObjectID, SmallVector<LockEnter, 4>> LiveLockEnters;

  // Monotonically increasing counter, bumped on every push to any
  // LiveLockEnters[id]. Never reset within an Analyzer run — relative
  // ordering within any one snapshot is all that matters for the cascade
  // rule, and across loop fixpoint iterations the inherited snapshots
  // already carry their own (older) Order tags, so a fresh push on
  // re-processing gets a strictly larger value without disturbing the
  // ordering of stale entries.
  uint32_t NextLockEnterOrder = 0;

  // Stable-across-iterations BytecodeDepth fallback. When a
  // monitorenter call lacks the `!jeandle.lock_depth` metadata, we need a
  // depth value that (a) is a meaningful proxy for cascade decisions within
  // a single processBlock walk, AND (b) does NOT change across loop-fixpoint
  // re-pushes of the same call. The Analyzer-run-monotonic NextLockEnterOrder
  // satisfies (a) but fails (b) — every re-push at iteration N+1 advances
  // the counter, which would mutate ObjectState::Locks[i].BytecodeDepth and
  // break the structural ObjectState::equivalentTo check used by the loop
  // fixpoint convergence path (via PEABlockState::equivalentTo).
  //
  // Solution: cache the FIRST NextLockEnterOrder value ever assigned to each
  // call site and reuse it on subsequent visits. The cache survives loop
  // snapshot/restore (it's an Analyzer-run-wide property of the call site,
  // not per-block state) and is only consulted in the metadata-absent path.
  DenseMap<CallBase *, uint32_t> FallbackBytecodeDepth;

  // Per-path "this object has already been materialized somewhere upstream"
  // set. Materialization is recorded at most once per ObjectID per pred path
  // — the first escape site wins; multi-pred merges that see the object
  // materialized on every incoming carry the Materialized state forward.
  DenseSet<jeandle::ObjectID> Materialized;

  // Per-block exit snapshots, keyed by the block that produced them.
  DenseMap<BasicBlock *, BlockExitInfo> BlockExits;

  // Function-wide dedup of (Pred, ObjectID) materializations. Multiple
  // merge-time Materialize-at-pred emissions for the same (Pred, ObjectID)
  // would otherwise produce duplicate invokes; this set ensures we emit
  // exactly one Materialize effect per (Pred, ObjectID) pair across the
  // entire run.
  DenseMap<BasicBlock *, DenseSet<jeandle::ObjectID>> MaterializedAtPred;

  // Per-merge-block deferred CreatePHI effects. mergeStates pushes
  // every CreatePHI it would have committed directly onto this list (keyed
  // by merge block); processBlock drains the list at the END of the block
  // walk, assigning each effect a FRESH nextSeqNo() at drain time so it is
  // strictly greater than any per-pred Materialize emitted during the same
  // processBlock call. This fixes the self-loop ordering bug: when the
  // merge block IS its own back-edge predecessor, both the CreatePHI and a
  // per-pred Materialize land in BlockEffects[BB], and the previous
  // emit-order SeqNo assignment made CreatePHI sort FIRST — leaving the
  // PHI's back-edge incoming wired to the OrigAlloc placeholder which then
  // becomes `poison` after EliminateAllocation. With drain-time SeqNo
  // assignment, CreatePHI always sorts AFTER per-pred Materialize within
  // the same block, so MatPerBlock is already populated when CreatePHI
  // wires its incomings.
  DenseMap<BasicBlock *, SmallVector<jeandle::PEAResult::Effect, 4>>
      PendingMergePhis;

  // Current analyzer mode. Flipped to MaterializeAll by
  // processLoop's overflow retry, or to StopNewInLoopNest at top-level
  // processLoop entry on a nest deeper than JeandlePEALoopCutoff, then
  // reverted before processLoop returns.
  Mode CurrentMode = Mode::Regular;

  // 2-stage MATERIALIZE_ALL escalation tracking. Set true the
  // first time a top-level processLoop escalates from Regular to
  // MATERIALIZE_ALL. If the MATERIALIZE_ALL body pass STILL fails to
  // converge, the second observation triggers a hard bail (unreachable in
  // debug, return without commit in release). Cleared on every top-level
  // processLoop entry to keep the bound per-nest.
  bool TooManyIterationsSeen = false;

  // Cross-recursion overflow signal. When a NESTED processLoop
  // recursion enters MATERIALIZE_ALL mode and is about to materialise a
  // VO whose AllocationCall is OUTSIDE the current loop (i.e. it belongs
  // to an outer loop's preheader-virtual set), set this flag so the
  // outer-most processLoop's body iteration breaks out to the
  // MATERIALIZE_ALL escalation path. Cleared on every top-level
  // processLoop entry. (Jeandle is C++ -fno-exceptions; a polled flag is
  // equivalent since processLoop is the only consumer.)
  bool OverflowFlag = false;

  // Currently-processed Loop (innermost). processLoop pushes /
  // pops this on entry / exit; materializeAt and materializeAtPred consult
  // it to decide whether a touched VO belongs to an outer scope (escape
  // upward from a nested MATERIALIZE_ALL iteration), in which case the
  // OverflowFlag is set. nullptr when no loop is active.
  Loop *CurrentProcessLoop = nullptr;

  // Per-block list of VOs registered while CurrentMode was
  // MaterializeAll. processBlock drains this list at end-of-block (after
  // the instruction walk but before snapshotExitState) and emits a
  // Materialize effect for each VO at the block's terminator. The
  // deferred emission lets all intra-block loads/stores fold against
  // FieldStates first; the materialise then captures the final field
  // values. Cleared per block by resetPerBlockState semantics — but the
  // bucket is keyed by BasicBlock so siblings don't collide.
  DenseMap<BasicBlock *, SmallVector<jeandle::ObjectID, 4>>
      PendingMaterializeAllVOs;

  // Stable allocation-site -> ObjectID cache. tier1Allocate consults
  // this BEFORE creating a fresh VO so re-processing the same allocation
  // call inside a loop fixpoint iteration reuses the original ID. Without
  // this the convergence check (which compares FieldStates and Virtuals
  // sets across iterations) would diverge forever — each iter would mint a
  // fresh ID for the same body-local alloc. The invariant is that
  // virtualizing the same allocation always yields the same ObjectID.
  // This cache is monotonically grown (never cleared across iterations
  // or runs of the analyzer).
  DenseMap<CallBase *, jeandle::ObjectID> AllocSiteToVO;

  // Loops for which the fixpoint did NOT converge AND the
  // MATERIALIZE_ALL preheader-force-materialize fallback was applied. The
  // safety net materializeBeforeLoops() skips these (their preheader's
  // virtuals were already drained).
  DenseSet<Loop *> PessimisticLoops;

  // Loops whose body fixpoint successfully converged. The safety net
  // materializeBeforeLoops() skips these too — virtuals that survived the
  // fixpoint at the preheader were tracked across the back-edge by
  // construction, and any in-body escape is already covered by the
  // per-instruction materializeAt that triggered inside the body during
  // the converged iteration. Forcing them to materialize at the preheader
  // would defeat the whole point of the loop fixpoint for the alloc-before-loop-with-body-
  // mutation pattern that today's behaviour cannot recover.
  DenseSet<Loop *> ConvergedLoops;

  // Every Loop* on which processLoop was invoked (including
  // recursive sub-loop calls). The safety-net
  // materializePreheaderVirtualsForUnvisitedLoops() drains preheader
  // virtuals ONLY for loops absent from this set, i.e. truly unvisited
  // loops (an unreachable top-level loop the RPO walk skipped, or a
  // sub-loop whose containing top-level loop's processLoop bailed before
  // recursing into it). An earlier gate used PessimisticLoops +
  // ConvergedLoops, which conflated "processLoop visited and handled" with
  // "processLoop never ran", missing the rare unvisited case where the
  // preheader drain is the only sound source of materialisation.
  DenseSet<Loop *> VisitedLoops;

  // Per-loop-header field-PHI cache. Keyed on (Header, ID, Offset). The
  // cache returns a STABLE PHINode* across fixpoint iterations so the
  // convergence check on BlockExitInfo.FieldStates can compare FieldValues
  // by Value pointer (otherwise every iteration would synthesize a fresh
  // PHI and the fixpoint would never close). Offset == -1 (i.e. the magic
  // VirtualObject::ArrayLengthSlotOffset value, also used as a sentinel
  // here for the "merged materialized pointer" PHI in the all-materialized
  // merge branch) is overloaded as the cache key for the materialized-ptr
  // PHI; offsets >= 0 are field PHIs. The cached PHI lives in
  // Result.OwnedLoopFieldPhis, which is preserved across rollback (unlike
  // OwnedPhis, which is truncated). The CreatePHI Effect referencing the
  // cached PHI is re-emitted in BlockEffects[Header] on every iteration —
  // BlockEffects[Header] is wiped on rollback, but the PHI itself is not.
  struct LoopPhiKey {
    BasicBlock *Header;
    jeandle::ObjectID ID;
    int64_t Offset; // -1 for the merged-materialized-ptr PHI
    bool operator==(const LoopPhiKey &O) const {
      return Header == O.Header && ID == O.ID && Offset == O.Offset;
    }
  };
  struct LoopPhiKeyHash {
    size_t operator()(const LoopPhiKey &K) const {
      return static_cast<size_t>(
          hash_combine(hash_value(K.Header), K.ID, K.Offset));
    }
  };
  std::unordered_map<LoopPhiKey, PHINode *, LoopPhiKeyHash> LoopFieldPhiCache;

  // Set of all loop-header blocks in F (populated up-front from LI). Used
  // by the merge code to gate the LoopFieldPhiCache lookup so non-loop
  // merges keep their current single-shot PHI behaviour.
  DenseSet<BasicBlock *> LoopHeaderSet;

  // Per-VO record of LLVM pointer-PHIs that processBlockPhis
  // aliased via Case-B (every incoming agrees on the same ObjectID).
  // commit() consults this to schedule explicit PHI erasures for VOs
  // that end up NeverEscapes, so EliminateAllocation isn't left with
  // a dead-but-still-parented `phi [poison, ..., poison]` survivor
  // in the IR.
  DenseMap<jeandle::ObjectID, SmallVector<llvm::PHINode *, 2>>
      CaseBPhiAliases;

  // Monotonicity guard for per-loop fixpoint iterations. For the loop
  // currently being processed, this records every loop block that has had a
  // BlockExits entry on at least one iteration. The fixpoint invariant is
  // that once a loop block has produced exit state, subsequent iterations
  // must continue
  // to produce exit state for that block (the convergence comparison is
  // structural; a dropped key on iter N+1 would otherwise be observed as a
  // structural change forever, never converging). Cleared at the end of
  // each top-level processLoop call to keep the map scoped to one loop.
  DenseMap<BasicBlock *, bool> KnownAliveLoopEnds;

  // Returns a stable PHI for the given (loop header, ID, offset) tuple,
  // creating one (and registering it in OwnedLoopFieldPhis) on first use.
  // Falls back to createUnparentedPhi (i.e. the legacy OwnedPhis path) when
  // BB is not a loop header.
  PHINode *getOrCreateLoopFieldPhi(BasicBlock *BB, jeandle::ObjectID ID,
                                   int64_t Offset, Type *Ty, unsigned N,
                                   const Twine &Name);

  void processBlock(BasicBlock *BB);
  void applyThreeTier(Instruction *I);

  // Per-block state helpers.
  void resetPerBlockState();
  void inheritFromExit(const BlockExitData &Exit);
  void mergeStates(BasicBlock *BB);
  void snapshotExitState(BasicBlock *BB);
  // Mirror of snapshotExitState that writes the per-object snapshot into
  // an arbitrary BlockExitData (rather than into BlockExits[BB]). Used by
  // processBlock to capture the pre-invoke state for the unwind variant.
  void snapshotExitStateInto(BlockExitData &Data);
  void processBlockPhis(BasicBlock *BB);

  // Resolve the per-pred BlockExitData a successor block should
  // inherit. Returns nullptr when the predecessor's contribution is dead
  // for this successor (either the pred has not been processed yet OR the
  // pred's terminator is an InvokeInst whose unwind edge was killed
  // because the invoke was virtualized away). Otherwise returns either
  // the pred's normal exit data (base data) or — when the successor is
  // the pred's invoke's unwind dest AND a pre-invoke snapshot was
  // recorded — the pre-invoke unwind variant.
  BlockExitData *exitDataFor(BasicBlock *Pred, BasicBlock *Succ);

  // PHI Case C: synthesize a merged VirtualObject when every incoming of a
  // pointer-PHI resolves to a DIFFERENT but COMPATIBLE virtual ID. Returns
  // true on success (the new VO is registered, the PHI is aliased to it, and
  // per-entry CreatePHI effects are emitted); false if compatibility,
  // identity, or per-entry type checks fail — caller falls through to the
  // Case A per-pred materialize path. Restricted to non-self-reference,
  // non-byte-array, non-two-slot entry cases.
  bool synthesizeCaseC(BasicBlock *BB, PHINode *Phi,
                       ArrayRef<std::optional<jeandle::ObjectID>> InIDs);

  // Loop-header cache for Case C — keyed on (mergeBB, sorted-source-IDs).
  // The cache exists so an iterative merge stabilization re-visiting a
  // loop header doesn't synthesize a fresh VO every iteration (otherwise
  // VirtualObjects grows unboundedly and the fixpoint never closes). Cache
  // value is the synthesized VO id; the caller looks it up and reuses the
  // existing VO + alias rather than calling createVirtualObject.
  //
  // Under processLoop's body fixpoint the cache hits on iter >= 1: it
  // survives across iterations (not snapshotted by take/restoreLoopSnapshot)
  // so the same synthetic VO ID is reused at the header. Combined with
  // LoopFieldPhiCache (stable per-offset PHI shells), this keeps FieldStates
  // structurally equal across iterations, which the convergence check
  // (loopBlockExitsEquivalent) requires.
  struct CaseCKey {
    BasicBlock *Block;
    SmallVector<jeandle::ObjectID, 4> SourceIDs;
    bool operator==(const CaseCKey &O) const {
      return Block == O.Block && SourceIDs == O.SourceIDs;
    }
  };
  struct CaseCKeyHash {
    size_t operator()(const CaseCKey &K) const {
      hash_code H = hash_value(K.Block);
      for (jeandle::ObjectID ID : K.SourceIDs)
        H = hash_combine(H, ID);
      return static_cast<size_t>(H);
    }
  };
  std::unordered_map<CaseCKey, jeandle::ObjectID, CaseCKeyHash> CaseCVOCache;

  PHINode *createUnparentedPhi(Type *Ty, unsigned N, const Twine &Name);

  // Produce a Value* of type LoadTy semantically equal to V, possibly
  // synthesizing an unparented coercion instruction (registered with
  // Result.OwnedInsts) that the transform's ReplaceLoad handler will splice
  // in immediately before the load. Returns V unchanged if no coercion is
  // needed, or nullptr if no safe coercion exists; callers should bail to
  // ineligible in the latter case. InsertContext is the load whose DebugLoc
  // (if any) is propagated onto the synthesized cast.
  //
  // WithinSlotByteOff is the byte offset of the load *within* the field-state
  // slot that holds V (i.e. LoadOffsetFromAlloc - EntryOffsetFromAlloc). It is
  // used for sub-bit-width integer loads: when EntryWidth > LoadWidth and
  // both sides are integer-typed, an `lshr` (by WithinSlotByteOff*8) is
  // emitted before a `trunc`. Little-endian byte order assumed (x86/aarch64).
  Value *coerceToType(Value *V, Type *LoadTy, Instruction *InsertContext,
                      int64_t WithinSlotByteOff = 0);

  // Why a materialization was emitted. Used to bump the
  // per-reason Statistic counter at the emission site. Cascade / Nested /
  // Unknown are not surfaced as standalone counters — they roll into the
  // total `JeandlePEAMaterialized` only.
  enum class MatReason : uint8_t {
    Unknown,
    Unhandled,   // unhandled escape-point instruction (generic
                 // "virtualize returned false" path).
    Merge,       // state merge: mixed virtual/materialized at a multi-pred
                 // entry, or lock-count cascade at merge.
    LoopExit,    // force-drain at a loop preheader.
    PHI,         // Case-A LLVM PHI fallback or Case-C synthetic-VO cascade.
    Cascade,     // sibling-virtual cascade triggered by strict lock order.
    Nested,      // recursive prerequisite materialization for an inner VO.
  };

  // Loop-soundness helpers.
  // Runs the preheader-drain ONLY on loops absent from
  // VisitedLoops (those never reached by processLoop). All other loops
  // are handled by processLoop directly: convergence (no drain needed),
  // pessimistic fallback (already drained inline), or
  // the overflow-recovery path which calls processStateBeforeLoopOnOverflow.
  void materializePreheaderVirtualsForUnvisitedLoops();

  // Drain every still-virtual VO at the loop preheader's
  // terminator. Used by the overflow-recovery path to forcibly demote any
  // outer VO that an inner MATERIALIZE_ALL iteration tried to touch, so
  // the outer fixpoint can re-run cleanly. Idempotent; safe to call
  // multiple times.
  void processStateBeforeLoopOnOverflow(Loop *L);

  // At every exit block of L, force-materialise still-virtual VOs
  // when the exit successor is an EH-pad block (landingpad / catchpad /
  // cleanuppad), to keep exception handlers from observing a
  // partially-materialised state. Downstream "deopt-bundle within reach"
  // detection is deferred because Jeandle currently has no deopt
  // machinery. Called from processLoop after convergence.
  void processLoopExit(Loop *L);
  // SkipGlobalRAUW=true marks the emitted Materialize as IsPerPred so the
  // transform does not RAUW OrigAlloc globally — used by the lock-cascade
  // path when multiple per-pred materializations of the same OrigAlloc coexist.
  void materializeAtPredFromExitInfo(jeandle::ObjectID ID, BasicBlock *PH,
                                     BlockExitData &ExitInfo,
                                     bool SkipGlobalRAUW = false,
                                     MatReason Reason = MatReason::Merge);

  // Real loop fixpoint. processLoop runs the fixpoint over L (which
  // includes its sub-loops; nested loops are dispatched recursively when
  // their header is encountered in the RPO walk). On convergence, all blocks
  // of L have their BlockExits populated and the outer RPO walk continues.
  // On failure, restores to the pre-loop snapshot, then runs the legacy
  // pessimistic path: force-materialize at preheader (drains every virtual)
  // and process the body once in MATERIALIZE_ALL mode (no new VOs created
  // inside).
  void processLoop(Loop *L);

  // Helpers used exclusively by processLoop.
  void processLoopBodyOnePass(Loop *L,
                              llvm::SmallPtrSetImpl<BasicBlock *> &OuterDone);

  // The per-iteration snapshot. All members are independently restorable.
  //
  // Eligible MUST be rolled back across loop-fixpoint iterations.
  // Numerous in-body paths (mergeStates' incompatible-merge bail, store
  // overlap, atomic / cmpxchg / memcpy fallbacks, etc.) mutate
  // Eligible[ID] = false unconditionally. Without snapshotting, a transient
  // escape pattern in iter N (e.g. an as-yet-unpopulated empty back-edge
  // exit) would permanently wedge the VO ineligible for iter N+1, even
  // though iter N+1's fully-populated back-edge would otherwise allow
  // re-virtualisation. Each iteration's per-VO ineligibility flips are
  // local to that iteration and only persist if the converged exit state
  // requires them.
  struct LoopSnapshot {
    jeandle::PEABlockState CurrentState;
    jeandle::AliasMap Aliases;
    DenseMap<jeandle::ObjectID, DenseMap<int64_t, jeandle::FieldValue>>
        FieldStates;
    DenseMap<jeandle::ObjectID, unsigned> LockCounts;
    DenseMap<jeandle::ObjectID, SmallVector<LockEnter, 4>> LiveLockEnters;
    DenseSet<jeandle::ObjectID> Materialized;
    DenseMap<jeandle::ObjectID, bool> EligibleSnapshot;
    // Number of VOs that existed BEFORE this iteration. IDs greater
    // than or equal to this index were created within the rolled-back iter;
    // restoreLoopSnapshot re-marks them Eligible[ID]=true so the next iter's
    // AllocSiteToVO cache-hit path (which bails on !Eligible.lookup) sees a
    // re-eligible VO and proceeds. Without this, body-local allocations
    // created inside the loop would be wedged ineligible across every
    // iteration after the first.
    size_t PreIterVOCount = 0;
    uint32_t NextSeqNo = 0;
    size_t OwnedPhisSize = 0;
    size_t OwnedInstsSize = 0;

    // For each block in L, the prior BlockExits[BB] (if any) and
    // BlockEffects[BB] (if any), captured *before* the loop iteration began.
    DenseMap<BasicBlock *, BlockExitInfo> SavedBlockExits;
    DenseMap<BasicBlock *, SmallVector<jeandle::PEAResult::Effect, 16>>
        SavedBlockEffects;
    DenseMap<BasicBlock *, DenseSet<jeandle::ObjectID>>
        SavedMaterializedAtPred;
    DenseSet<BasicBlock *> HadBlockExits;
    DenseSet<BasicBlock *> HadBlockEffects;
    DenseSet<BasicBlock *> HadMaterializedAtPred;
  };

  void takeLoopSnapshot(Loop *L,
                        const llvm::SmallPtrSetImpl<BasicBlock *> &LoopBlocks,
                        LoopSnapshot &S);
  void restoreLoopSnapshot(
      const llvm::SmallPtrSetImpl<BasicBlock *> &LoopBlocks,
      const LoopSnapshot &S);

  // Convergence: structural equivalence of two snapshots of (loop block ->
  // BlockExitInfo).
  bool loopBlockExitsEquivalent(
      const llvm::SmallPtrSetImpl<BasicBlock *> &LoopBlocks,
      const DenseMap<BasicBlock *, BlockExitInfo> &A,
      const DenseMap<BasicBlock *, BlockExitInfo> &B) const;

  static bool blockExitInfoEquivalent(const BlockExitInfo &A,
                                      const BlockExitInfo &B);
  // Structural equivalence of just the BlockExitData base (the per-object
  // book-keeping). Factored out so blockExitInfoEquivalent can compare
  // both the base data and the optional UnwindData snapshot.
  static bool exitDataEquivalent(const BlockExitData &A,
                                 const BlockExitData &B);

  void tier1Allocate(CallBase *CB);
  // Returns true iff the store was consumed as a virtualised store
  // (pointer side resolved to a virtual base, regardless of whether the
  // value side made the VO ineligible). Returns false when the pointer
  // operand is NOT a virtual ref; the caller then falls through to the
  // generic hasVirtualInputs / materializeAllVirtualOperands path so a
  // VALUE-side virtual is not silently leaked to a global / non-virtual
  // pointer (e.g. `store ptr %virt, ptr @G`).
  bool tier2Store(StoreInst *SI);
  void tier2Load(LoadInst *LI);
  // TODO: tier2AtomicRMW / tier2CmpXchg — re-add with the jeandle-jdk
  // Unsafe atomic intrinsic inliner.
  // TODO: tier2ArrayCopy — re-add with System.arraycopy inliner
  // (llvm.memcpy / llvm.memmove emission).
  // TODO: tier2MemSet — re-add with a frontend producer of llvm.memset on
  // user-visible arrays (e.g. Arrays.fill).
  bool tier2JavaOpFold(CallBase *CB);
  bool foldArrayLength(CallBase *CB);
  bool foldLoadKlass(CallBase *CB);
  bool foldCheckCast(CallBase *CB);
  bool foldCheckInstanceOf(CallBase *CB);
  bool foldInstanceOf(CallBase *CB);
  bool foldMonitorEnter(CallBase *CB);
  bool foldMonitorExit(CallBase *CB);
  bool foldArrayStoreCheck(CallBase *CB);
  bool foldCheckIfValueBased(CallBase *CB);
  bool foldRegisterFinalizerIfNeeded(CallBase *CB);
  // Common helper for checkcast/check_instanceof: returns the folded constant
  // bool (true/false) if the relationship is statically known, or
  // std::nullopt otherwise.
  std::optional<bool> evalSubtypeRelation(uintptr_t SubKlass,
                                          uintptr_t SuperKlass);
  void emitReplaceCall(CallBase *CB, Value *Replacement, jeandle::ObjectID ID);
  void materializeAllVirtualOperands(Instruction *I);
  void materializeAt(jeandle::ObjectID ID, Instruction *InsertBefore,
                     MatReason Reason = MatReason::Unhandled);
  void propagatePointerAlias(Instruction *I);

  // Walk every other VO's FieldStates in the supplied map and rewrite
  // any FieldValue::virtualRef(FlippedID) entry to MaterializedRef(NewPtr).
  // Called after a VO is flipped to Materialized so sibling VOs that hold a
  // reference to it learn about the materialized pointer immediately. Without
  // this sweep, a later store/load through a sibling's field that still
  // carries VirtualRef(FlippedID) would fire the transform-side assertion
  // ("VirtualRef field entries must have been rewritten to MaterializedRef
  // during analysis") in debug, or silently drop the field in release.
  void updateOtherStatesForMaterialized(
      jeandle::ObjectID FlippedID, Value *NewPtr,
      DenseMap<jeandle::ObjectID, DenseMap<int64_t, jeandle::FieldValue>> &FS);

  // Bump the appropriate per-reason Statistic. Always bumps the
  // total JeandlePEAMaterialized; the per-reason counter is bumped only for
  // the four reasons (Unhandled / Merge / LoopExit / PHI).
  static void bumpMaterializeStat(MatReason R);

  void commit();
  void dropEffectsFor(jeandle::ObjectID ID);
};

void Analyzer::processBlock(BasicBlock *BB) {
  // Rebuild per-block state from the predecessor snapshots before we walk
  // instructions. Entry block starts empty. Single-pred blocks inherit.
  // Multi-pred blocks run the merge (mergeStates also handles the degenerate
  // "no processed preds" case, which can happen e.g. on irreducible loop
  // headers where the back-edge pred hasn't been visited yet — we treat
  // those conservatively as starting empty).
  // Statically-unreachable blocks (constant-condition dead arms, blocks
  // unreachable from entry) are pruned by the pre-PEA SimplifyCFG pass in
  // buildJeandlePipeline, so we never see them here.

  resetPerBlockState();
  if (BB == &F.getEntryBlock()) {
    // Entry: nothing to inherit; per-block state is empty.
    processBlockPhis(BB);
  } else if (BB->hasNPredecessors(1)) {
    BasicBlock *P = *predecessors(BB).begin();
    if (BlockExitData *ED = exitDataFor(P, BB))
      inheritFromExit(*ED);
    processBlockPhis(BB);
  } else {
    // mergeStates wraps BOTH the per-VO loop AND the PHI loop in a
    // single do-while so a Case-C synthesis or Case-A fallback in
    // processBlockPhis that materialises an inner VO at a pred can
    // reawaken the per-VO decisions (which depended on the now-stale
    // pred-side virtuality). mergeStates calls processBlockPhis itself.
    mergeStates(BB);
  }

  // Exception edge state splitting. If the block ends in an InvokeInst,
  // snapshot the per-object state immediately BEFORE applying the invoke.
  // The post-invoke state (the regular snapshotExitState below) is what
  // the normal successor inherits; the snapshot we take here becomes the
  // unwind successor's inheritance (the materialize logically happens
  // during the call, so the handler should not see partially-materialized
  // state).
  //
  // We only bother when the function has a personality (no personality =>
  // no real exception handlers; the work would be observably inert) and
  // the terminator actually IS an InvokeInst. Both conditions are required
  // for state-splitting to matter.
  InvokeInst *TermII = dyn_cast<InvokeInst>(BB->getTerminator());
  bool MaybeSplit = TermII && F.hasPersonalityFn();
  std::optional<BlockExitData> PreInvokeSnapshot;
  // Drain any VOs registered under MATERIALIZE_ALL in this
  // block, materialising each at the terminator IP. Called immediately
  // before the terminator instruction is processed so the materialise
  // takes effect BEFORE PreInvokeSnapshot (so both normal and unwind
  // successors inherit a Materialized-VO view, never a stale Virtual
  // view that would dangling-ref an OrigAlloc that the transform RAUW'd
  // away). Idempotent (clears the per-block list).
  auto drainMaterializeAll = [&]() {
    auto PMIt = PendingMaterializeAllVOs.find(BB);
    if (PMIt == PendingMaterializeAllVOs.end())
      return;
    SmallVector<jeandle::ObjectID, 4> ToMat = std::move(PMIt->second);
    PendingMaterializeAllVOs.erase(PMIt);
    llvm::sort(ToMat); // deterministic order
    Instruction *IP = BB->getTerminator();
    for (jeandle::ObjectID ID : ToMat)
      materializeAt(ID, IP, MatReason::Unhandled);
  };

  for (Instruction &I : *BB) {
    if (&I == BB->getTerminator())
      drainMaterializeAll();
    if (MaybeSplit && &I == TermII) {
      PreInvokeSnapshot.emplace();
      snapshotExitStateInto(*PreInvokeSnapshot);
    }
    applyThreeTier(&I);
  }
  // Defensive: empty blocks or blocks whose terminator was never enqueued
  // through the loop above still need a drain (the foreach loop above does
  // the right thing for any non-empty block, but a future change might
  // skip-over the terminator).
  drainMaterializeAll();

  snapshotExitState(BB);

  if (TermII) {
    BlockExitInfo &Info = BlockExits[BB];
    Info.TerminatorInvoke = TermII;
    Info.UnwindDest = TermII->getUnwindDest();

    // Detect whether the terminator-invoke was virtualized away by the
    // analyzer (e.g. tier2JavaOpFold emitted a ReplaceCall effect on it).
    // The transform rewrites such invokes as an unconditional branch to
    // the normal successor, so from PEA's perspective the unwind edge no
    // longer exists. Mark the edge killed so exitDataFor reports no
    // contribution to the unwind successor.
    bool Virtualized = false;
    auto EIt = Result.BlockEffects.find(BB);
    if (EIt != Result.BlockEffects.end()) {
      for (const auto &E : EIt->second) {
        if (E.Target == TermII &&
            E.Kind == jeandle::PEAResult::EffectKind::ReplaceCall) {
          Virtualized = true;
          break;
        }
      }
    }
    Info.UnwindEdgeKilled = Virtualized;

    // Only stash the pre-invoke snapshot if (a) we actually took one
    // (function has a personality), (b) the invoke wasn't virtualized
    // (the kill flag already handles that case), AND (c) the snapshot
    // actually differs from the post-invoke base data. The last gate
    // avoids paying the DenseMap-copy cost (and the convergence-check
    // cost downstream) for invokes that triggered no PEA state change.
    if (!Virtualized && PreInvokeSnapshot &&
        !exitDataEquivalent(Info, *PreInvokeSnapshot))
      Info.UnwindData = std::move(PreInvokeSnapshot);
  }

  // Drain deferred CreatePHI effects emitted during this block's
  // mergeStates / processBlockPhis / synthesizeCaseC, assigning each a
  // FRESH SeqNo at drain time so it sorts strictly AFTER any per-pred
  // Materialize emitted during the instruction walk above (the self-loop
  // ordering bug fix). Cross-block ordering is unchanged because the per-
  // pred Materialize lives in a different BlockEffects bucket from the
  // CreatePHI; the SeqNo assigned here is irrelevant to cross-block
  // ordering (RPO drives that), but consistent across runs because
  // nextSeqNo is monotonic within a single Analyzer::run.
  auto It = PendingMergePhis.find(BB);
  if (It != PendingMergePhis.end()) {
    for (auto &PE : It->second) {
      PE.SeqNo = Result.nextSeqNo();
      Result.addBlockEffect(std::move(PE));
    }
    PendingMergePhis.erase(It);
  }
}

void Analyzer::resetPerBlockState() {
  CurrentState = jeandle::PEABlockState();
  FieldStates.clear();
  LockCounts.clear();
  LiveLockEnters.clear();
  Materialized.clear();
}

BlockExitData *Analyzer::exitDataFor(BasicBlock *Pred, BasicBlock *Succ) {
  auto It = BlockExits.find(Pred);
  if (It == BlockExits.end())
    return nullptr;
  BlockExitInfo &Info = It->second;
  // When the pred ends in an InvokeInst whose unwind dest is `Succ`,
  // the unwind edge participates in state-splitting.
  if (Info.TerminatorInvoke && Info.UnwindDest == Succ) {
    if (Info.UnwindEdgeKilled) {
      // Invoke was virtualized; the handler is unreachable for analysis.
      // Return nullptr so the merge consumer drops this pred entirely.
      return nullptr;
    }
    if (Info.UnwindData)
      return &*Info.UnwindData;
  }
  // Normal successor (or unwind successor with no state-split needed).
  return &Info;
}

void Analyzer::inheritFromExit(const BlockExitData &Exit) {
  for (jeandle::ObjectID ID : Exit.Virtuals) {
    if (!Eligible.lookup(ID))
      continue;
    CurrentState.addObject(ID, jeandle::ObjectState(/*numEntries=*/0));
  }
  for (jeandle::ObjectID ID : Exit.Materialized) {
    if (!Eligible.lookup(ID))
      continue;
    jeandle::VirtualObject &VObj = *Result.VirtualObjects[ID];
    jeandle::ObjectState OS(0);
    // Prefer the snapshot's MaterializedValues entry (e.g. a merge-block
    // PHI synthesized by an earlier mergeStates) over the OrigAlloc fallback.
    Value *MV = nullptr;
    auto MIt = Exit.MaterializedValues.find(ID);
    if (MIt != Exit.MaterializedValues.end())
      MV = MIt->second;
    if (!MV)
      MV = VObj.AllocationCall;
    OS.materialize(MV);
    CurrentState.addObject(ID, std::move(OS));
    Materialized.insert(ID);
  }
  for (auto &Kv : Exit.FieldStates) {
    if (!Eligible.lookup(Kv.first))
      continue;
    FieldStates[Kv.first] = Kv.second;
  }
  for (auto &Kv : Exit.LockCounts) {
    if (!Eligible.lookup(Kv.first))
      continue;
    if (Kv.second != 0)
      LockCounts[Kv.first] = Kv.second;
  }
  // Inherit the live monitorenter stack alongside LockCounts. The
  // CallBase* identities are still valid (we never erase IR during analysis).
  for (auto &Kv : Exit.LiveLockEnters) {
    if (!Eligible.lookup(Kv.first))
      continue;
    if (!Kv.second.empty())
      LiveLockEnters[Kv.first] = Kv.second;
  }
  // Mirror the inherited live stack onto each per-VO ObjectState's
  // Locks so the on-VO view matches the analyzer-side DenseMap from the
  // first instruction of this block. Without this, ObjectState::Locks would
  // be empty for an inherited VO until the first foldMonitorEnter in this
  // block, and a downstream equivalentTo comparison (e.g. merge fast path)
  // would over-collapse two VOs with structurally different lock states.
  for (auto &Kv : Exit.LiveLockEnters) {
    if (!Eligible.lookup(Kv.first))
      continue;
    if (!CurrentState.hasObjectState(Kv.first))
      continue;
    jeandle::ObjectState &OS =
        CurrentState.getObjectStateForModification(Kv.first);
    if (!OS.isVirtual())
      continue;
    for (const LockEnter &LE : Kv.second)
      OS.addLock({LE.Call, LE.BytecodeDepth});
  }
}

PHINode *Analyzer::createUnparentedPhi(Type *Ty, unsigned N,
                                       const Twine &Name) {
  PHINode *Phi = PHINode::Create(Ty, N, Name);
  Result.OwnedPhis.emplace_back(Phi);
  return Phi;
}

PHINode *Analyzer::getOrCreateLoopFieldPhi(BasicBlock *BB, jeandle::ObjectID ID,
                                            int64_t Offset, Type *Ty,
                                            unsigned N, const Twine &Name) {
  // Outside a loop header, fall back to the legacy single-shot OwnedPhis path.
  if (!LoopHeaderSet.count(BB))
    return createUnparentedPhi(Ty, N, Name);

  // At a loop-header BB, size the PHI shell for the block's FULL
  // fan-in (every predecessor — forward edge AND back edge), regardless of
  // how many incomings the caller has visited on this particular iteration.
  // Without this, iter 1 (which only sees forward-edge preds before the
  // body has been walked) reserves capacity 1; iter 2's caller hands N=2
  // and PHINode::reserveOperand has to grow the operand list — wasted work
  // and (defensively) avoids a stale-capacity surprise if the IR has more
  // preds than the analyzer thinks. BB->getNumPredecessors() observes ALL
  // preds even on first-pass analysis.
  unsigned FullN = static_cast<unsigned>(
      std::distance(pred_begin(BB), pred_end(BB)));
  if (FullN < N)
    FullN = N; // caller knows of more than IR shows; should not occur on
               // well-formed IR but cheap to honour.

  LoopPhiKey K{BB, ID, Offset};
  auto It = LoopFieldPhiCache.find(K);
  if (It != LoopFieldPhiCache.end()) {
    PHINode *Cached = It->second;
    // Defensive: WeakTrackingVH would auto-null if the PHI was deleted via
    // some other code path. The cache entry is plain PHINode*, so revalidate.
    bool Valid = false;
    for (auto &VH : Result.OwnedLoopFieldPhis) {
      if (VH == Cached) { Valid = true; break; }
    }
    // Tightened validity check. During analysis the PHI is an
    // unparented shell — the transform pass is responsible for inserting it
    // into the merge block and calling addIncoming. Therefore a healthy
    // cached PHI has getNumIncomingValues() == 0 at every analysis-time
    // touch. Any non-zero count indicates a leak (e.g. an earlier code
    // path mistakenly called addIncoming on the shell) and we drop the
    // cache entry to force re-creation. Replaces the previous "<= N" guard
    // which was effectively no-op (count was always 0) and accepted stale
    // PHIs silently.
    if (Valid && Cached->getType() == Ty &&
        Cached->getNumIncomingValues() == 0) {
      // On cache hit, defensively clear any operands the shell
      // might have accumulated. Today's caller-driven incoming-list
      // construction passes Value*/BasicBlock* through the CreatePHI
      // Effect's PHIIncomingValues / PHIIncomingBlocks lists — never via
      // direct Phi->addIncoming() during analysis — so the loop below is
      // a no-op in practice. It's spelled out anyway to keep the
      // post-condition explicit ("cache hit returns an empty shell").
      while (Cached->getNumIncomingValues() > 0)
        Cached->removeIncomingValue(
            Cached->getNumIncomingValues() - 1u, /*DeletePHIIfEmpty=*/false);
      return Cached;
    }
    // Cache stale (rare): drop entry and fall through to recreate.
    LoopFieldPhiCache.erase(It);
  }

  PHINode *Phi = PHINode::Create(Ty, FullN, Name);
  Result.OwnedLoopFieldPhis.emplace_back(Phi);
  LoopFieldPhiCache[K] = Phi;
  return Phi;
}

// Type-coercion for tier2Load.
//
// Handles same-bit-width primitive↔primitive (BitCast), and sub-bit-width
// integer loads of a wider integer-stored slot (`lshr` by within-slot byte
// offset * 8, then `trunc`). Pointer↔primitive at the same slot is forbidden
// because the slot's reference-vs-primitive nature must be stable, so a
// ref cannot be re-read as a primitive (or vice-versa) without materializing.
// Cross-addrspace pointer coercion is also bailed (rare, GC-risky).
//
// Widening loads (EntryWidth < LoadWidth) bail: would require multi-slot
// read+concat which is not worth the complexity for sub-bit-width.
//
// Sub-byte loads (load size not a whole multiple of 8 bits, e.g. i1/i7) bail.
//
// Endianness: this code is LITTLE-ENDIAN-specific (x86, aarch64). For a
// big-endian target (e.g. RISC-V BE), the within-slot byte offset would need
// to be flipped to `(EntryByteSize - LoadByteSize - WithinSlotByteOff)`.
//
// Unparented coercion instructions are registered in Result.OwnedInsts; the
// transform's ReplaceLoad handler walks the operand chain and splices them
// before the target load (so the lshr precedes the trunc, both before the
// load).
Value *Analyzer::coerceToType(Value *V, Type *LoadTy,
                              Instruction *InsertContext,
                              int64_t WithinSlotByteOff) {
  Type *VTy = V->getType();
  if (VTy == LoadTy && WithinSlotByteOff == 0)
    return V;
  if (!VTy->isSized() || !LoadTy->isSized())
    return nullptr;
  uint64_t VBits = DL.getTypeSizeInBits(VTy);
  uint64_t LBits = DL.getTypeSizeInBits(LoadTy);

  // Stable-slot-kind invariant: ref↔primitive at the same slot must materialize.
  if (VTy->isPointerTy() != LoadTy->isPointerTy())
    return nullptr;

  // Pointer↔pointer: pointers don't truncate. Require exact same offset and
  // matching address spaces. Same-AS same-bitwidth pointers are already
  // type-identical under opaque pointers, so a true pointer coercion is rare;
  // defend against the cross-AS case.
  if (VTy->isPointerTy() && LoadTy->isPointerTy()) {
    if (WithinSlotByteOff != 0 || VBits != LBits)
      return nullptr;
    if (VTy->getPointerAddressSpace() != LoadTy->getPointerAddressSpace())
      return nullptr;
    return V;
  }

  // Both primitives from here. Sub-byte loads (e.g. i1) are bailed; supporting
  // them would need bit-granular shift/mask which we don't need today.
  if (LBits == 0 || (LBits % 8) != 0)
    return nullptr;
  if (VBits == 0 || (VBits % 8) != 0)
    return nullptr;

  // Same bit width → BitCast (Float↔Int, Half↔i16, etc.).
  if (VBits == LBits && WithinSlotByteOff == 0) {
    if (!CastInst::isBitCastable(VTy, LoadTy))
      return nullptr;
    Instruction *Cast = CastInst::Create(Instruction::BitCast, V, LoadTy,
                                         "pea.coerce",
                                         /*InsertBefore=*/nullptr);
    if (InsertContext)
      Cast->setDebugLoc(InsertContext->getDebugLoc());
    Result.OwnedInsts.emplace_back(Cast);
    return Cast;
  }

  // Sub-bit-width: EntryWidth > LoadWidth, both integer-typed, byte-aligned within the
  // slot. Emit `lshr V, WithinSlotByteOff*8` (omit if offset==0), then `trunc`
  // to LoadTy.
  if (VBits > LBits && VTy->isIntegerTy() && LoadTy->isIntegerTy()) {
    if (WithinSlotByteOff < 0)
      return nullptr;
    uint64_t ShiftBits = static_cast<uint64_t>(WithinSlotByteOff) * 8ULL;
    // The load must fit entirely within the slot's bytes.
    if (ShiftBits + LBits > VBits)
      return nullptr;

    Value *Src = V;
    if (ShiftBits != 0) {
      Constant *ShAmt = ConstantInt::get(VTy, ShiftBits);
      Instruction *Shr =
          BinaryOperator::Create(Instruction::LShr, V, ShAmt, "pea.coerce.shr",
                                 /*InsertBefore=*/nullptr);
      if (InsertContext)
        Shr->setDebugLoc(InsertContext->getDebugLoc());
      Result.OwnedInsts.emplace_back(Shr);
      Src = Shr;
    }
    Instruction *Trunc = CastInst::Create(Instruction::Trunc, Src, LoadTy,
                                          "pea.coerce.trunc",
                                          /*InsertBefore=*/nullptr);
    if (InsertContext)
      Trunc->setDebugLoc(InsertContext->getDebugLoc());
    Result.OwnedInsts.emplace_back(Trunc);
    return Trunc;
  }

  // EntryWidth < LoadWidth, or float/double mismatched with different widths,
  // or any other unhandled cross-kind/cross-width: bail. The caller will mark
  // the VO ineligible so the original alloc/store/load survive in IR.
  return nullptr;
}

void Analyzer::mergeStates(BasicBlock *BB) {
  // Collect the snapshots of every predecessor we've already processed. RPO
  // guarantees forward-edge preds are visited first; back-edge preds are not
  // yet available and are silently skipped (the loop-preheader force-
  // materialization sweep handles loop soundness).
  SmallVector<BasicBlock *, 4> PredBBs;
  SmallVector<BlockExitData *, 4> Preds;
  for (BasicBlock *P : predecessors(BB)) {
    // When P ends in an InvokeInst whose unwind dest is BB,
    // exitDataFor returns either the pre-invoke unwind variant (if
    // recorded) or nullptr (if the invoke was virtualized and the
    // unwind edge is dead — that pred contributes nothing).
    BlockExitData *ED = exitDataFor(P, BB);
    if (!ED)
      continue;
    PredBBs.push_back(P);
    Preds.push_back(ED);
  }
  if (Preds.empty()) {
    // Nothing to inherit; start empty. PHIs (if any) at this BB still need
    // walking — none of their incomings have a live pred contribution, so
    // processBlockPhis simply records nothing here, but we run it for
    // consistency with the non-degenerate paths.
    processBlockPhis(BB);
    return;
  }
  if (Preds.size() == 1) {
    inheritFromExit(*Preds[0]);
    // With a single live pred this is effectively a single-pred
    // block (all other preds were dead/killed). processBlockPhis still
    // has to alias / materialize the explicit LLVM PHIs of pointer type.
    processBlockPhis(BB);
    return;
  }

  // identicalExitData fast path. If every predecessor's
  // BlockExitData is byte-equivalent (same Virtuals, Materialized,
  // FieldStates per offset, LockCounts, LiveLockEnters,
  // MaterializedValues), the merge is degenerate: we can inherit from
  // preds[0] directly without paying the O(|preds| * |virtuals| *
  // |offsets|) per-VO merge cost.
  // We additionally require that no pred carries a different
  // OrigAlloc placeholder for the same materialized ID — preds[0] is
  // representative once that's true. processBlockPhis still runs
  // after the inherit to alias any pointer PHIs (Case-B uniform
  // ID across incomings is the common winning pattern at uniform
  // merges).
  {
    bool AllSame = true;
    for (unsigned i = 1; i < Preds.size(); ++i) {
      if (!exitDataEquivalent(*Preds[0], *Preds[i])) {
        AllSame = false;
        break;
      }
    }
    if (AllSame) {
      inheritFromExit(*Preds[0]);
      processBlockPhis(BB);
      return;
    }
  }

  // Compute the INTERSECTION of ObjectIDs that EVERY predecessor
  // tracks (either virtual or already materialized). Only IDs in the
  // intersection may remain unified at BB's entry. Any ID present on
  // some preds but not all is dropped
  // from per-VO consideration here: SSA guarantees that if such an ID
  // has uses past the merge, those uses flow through an explicit PHI
  // node, which processBlockPhis (Case-A fallback) handles by
  // materialising the virtual incomings at the per-pred terminator. An
  // ID that's loop-local and dead by the merge (Virtual at the back-edge
  // pred, absent at the forward-edge pred) is correctly dropped from
  // the per-VO loop and survives as a virtual-on-the-back-edge alloc
  // that the per-block EliminateAllocation effect kills entirely — at
  // a loop header the intersection is exactly the set that persists
  // into the loop body.
  DenseSet<jeandle::ObjectID> Intersect;
  for (jeandle::ObjectID ID : Preds[0]->Virtuals)
    Intersect.insert(ID);
  for (jeandle::ObjectID ID : Preds[0]->Materialized)
    Intersect.insert(ID);
  for (unsigned i = 1; i < Preds.size(); ++i) {
    DenseSet<jeandle::ObjectID> Tracked;
    for (jeandle::ObjectID ID : Preds[i]->Virtuals)
      Tracked.insert(ID);
    for (jeandle::ObjectID ID : Preds[i]->Materialized)
      Tracked.insert(ID);
    SmallVector<jeandle::ObjectID, 8> ToRemove;
    for (jeandle::ObjectID ID : Intersect)
      if (!Tracked.count(ID))
        ToRemove.push_back(ID);
    for (jeandle::ObjectID ID : ToRemove)
      Intersect.erase(ID);
  }
  SmallVector<jeandle::ObjectID, 8> IDs(Intersect.begin(), Intersect.end());
  llvm::sort(IDs); // deterministic order for ineligibility/marking effects.

  // Iterative merge stabilization. A nested materialize
  // triggered inside the per-field PHI synthesis path (when a per-pred slot
  // resolves to a VirtualRef whose target must be materialized at that pred)
  // can invalidate the per-VO decisions already made earlier in this loop:
  // another VO processed before the materialize might have decided to stay
  // virtual based on the inner's then-virtual state, but the inner is now
  // materialized on at least one pred, which downstream affects field-PHI
  // synthesis (type compatibility, value substitution) for any other VO whose
  // field references the inner.
  //
  // To converge to a `do { ... } while (materialized)` merge fixpoint,
  // we wrap the per-VO loop in a do-while that re-runs whenever any
  // materializeAtPredFromExitInfo call inside the loop did real work (an
  // Effect was emitted). State that's local to the merge — CurrentState,
  // FieldStates, LockCounts, LiveLockEnters, Materialized, Aliases,
  // OwnedPhis / OwnedInsts grown this iteration, and the merge BB's newly
  // appended BlockEffects — is snapshotted before each iteration and
  // restored on retry. State that captures the materialize side-effects —
  // each pred's BlockExitInfo (now showing the inner as Materialized), the
  // function-wide MaterializedAtPred dedup map, the per-pred BlockEffects
  // entries for the Materialize effect, and the Eligible flags (which a
  // legitimate materialize-bail may have flipped to false) — survives across
  // retries. The progress measure is monotone: every retry-triggering
  // materialize flips at least one (Pred, ID) entry from Virtual to
  // Materialized in ExitInfo, so the per-VO problem strictly shrinks. The
  // cap of 10 is purely a defensive safety net.
  struct MergeSnapshot {
    jeandle::PEABlockState CurrentState;
    DenseMap<jeandle::ObjectID, DenseMap<int64_t, jeandle::FieldValue>>
        FieldStates;
    DenseMap<jeandle::ObjectID, unsigned> LockCounts;
    DenseMap<jeandle::ObjectID, SmallVector<LockEnter, 4>> LiveLockEnters;
    DenseSet<jeandle::ObjectID> Materialized;
    jeandle::AliasMap Aliases;
    size_t OwnedPhisSize = 0;
    size_t OwnedInstsSize = 0;
    uint32_t NextSeqNo = 0;
    size_t MergeBBEffectSize = 0;
    // Rollback of PendingMergePhis[BB] size; truncate on retry so a
    // bailed iteration's deferred PHI emissions don't carry over and double-
    // commit at the end-of-processBlock drain.
    size_t PendingMergePhisSize = 0;
  };

  auto TakeSnapshot = [&](MergeSnapshot &S) {
    S.CurrentState = CurrentState;
    S.FieldStates = FieldStates;
    S.LockCounts = LockCounts;
    S.LiveLockEnters = LiveLockEnters;
    S.Materialized = Materialized;
    S.Aliases = Aliases.snapshot();
    S.OwnedPhisSize = Result.OwnedPhis.size();
    S.OwnedInstsSize = Result.OwnedInsts.size();
    S.NextSeqNo = Result.NextSeqNo;
    auto BIt = Result.BlockEffects.find(BB);
    S.MergeBBEffectSize =
        (BIt == Result.BlockEffects.end()) ? 0u : BIt->second.size();
    auto PIt = PendingMergePhis.find(BB);
    S.PendingMergePhisSize =
        (PIt == PendingMergePhis.end()) ? 0u : PIt->second.size();
  };

  auto RestoreSnapshot = [&](MergeSnapshot &S) {
    CurrentState = S.CurrentState;
    FieldStates = std::move(S.FieldStates);
    LockCounts = std::move(S.LockCounts);
    LiveLockEnters = std::move(S.LiveLockEnters);
    Materialized = std::move(S.Materialized);
    Aliases.restore(S.Aliases);
    // Pop and delete any unparented PHIs/insts added this iteration. An
    // already-parented (i.e. inserted into a BB by some side path) instruction
    // should not be deleted — but mergeStates only creates unparented PHIs via
    // createUnparentedPhi; insertion happens in the transform pass. So any
    // value added during this iteration is still unparented when we restore.
    while (Result.OwnedPhis.size() > S.OwnedPhisSize) {
      WeakTrackingVH &VH = Result.OwnedPhis.back();
      if (Value *V = VH) {
        if (auto *P = dyn_cast<PHINode>(V))
          if (!P->getParent())
            delete P;
      }
      Result.OwnedPhis.pop_back();
    }
    while (Result.OwnedInsts.size() > S.OwnedInstsSize) {
      WeakTrackingVH &VH = Result.OwnedInsts.back();
      if (Value *V = VH) {
        if (auto *I = dyn_cast<Instruction>(V))
          if (!I->getParent())
            I->deleteValue();
      }
      Result.OwnedInsts.pop_back();
    }
    Result.NextSeqNo = S.NextSeqNo;
    auto BIt = Result.BlockEffects.find(BB);
    if (BIt != Result.BlockEffects.end() &&
        BIt->second.size() > S.MergeBBEffectSize) {
      BIt->second.resize(S.MergeBBEffectSize);
    }
    // Roll back PendingMergePhis[BB] to its pre-iter size. The PHIs
    // owned by the dropped entries were already deleted above as part of
    // the OwnedPhis truncation (mergeStates only creates unparented PHIs
    // via getOrCreateLoopFieldPhi / createUnparentedPhi), so the now-stale
    // entries here would reference deleted PHINode pointers if we didn't
    // truncate.
    auto PIt = PendingMergePhis.find(BB);
    if (PIt != PendingMergePhis.end() &&
        PIt->second.size() > S.PendingMergePhisSize) {
      PIt->second.resize(S.PendingMergePhisSize);
    }
  };

  constexpr unsigned kMaxRetries = 10;
  unsigned Iter = 0;
  bool Changed = false;
  MergeSnapshot Snap;
  TakeSnapshot(Snap);

  do {
    if (Iter > 0) {
      // Discard partial work from the previous iteration and reset Snap so
      // the next iteration sees a clean state. ExitInfo/MaterializedAtPred/
      // per-pred Effects/Eligible all carry over by design.
      RestoreSnapshot(Snap);
      TakeSnapshot(Snap);
    }
    if (Iter >= kMaxRetries) {
      // Safety net. Cap reached — bail every VO in the working set so the
      // original IR survives at this merge. This indicates a pathology in
      // the input (or a bug in the convergence-measure proof).
      LLVM_DEBUG(dbgs() << "PEA: mergeStates retry cap (" << kMaxRetries
                        << ") reached at BB '" << BB->getName()
                        << "'; bailing all VOs at merge.\n");
      for (jeandle::ObjectID ID : IDs)
        Eligible[ID] = false;
      return;
    }
    ++Iter;
    Changed = false;

    for (jeandle::ObjectID ID : IDs) {
    if (!Eligible.lookup(ID))
      continue;

    // Per-pred disposition.
    bool AllVirtual = true;
    bool AllMaterialized = true;
    for (const auto *P : Preds) {
      bool V = P->Virtuals.count(ID);
      bool M = P->Materialized.count(ID);
      if (!V)
        AllVirtual = false;
      if (!M)
        AllMaterialized = false;
    }

    if (AllMaterialized) {
      // Every incoming path already materialized the object — carry the
      // materialized state forward at BB's entry. If the materialized pointer
      // differs across preds, synthesize a ptr addrspace(1) PHI; otherwise
      // reuse the unique pointer.
      jeandle::VirtualObject &VObj = *Result.VirtualObjects[ID];
      Value *Unique = nullptr;
      bool AllSame = true;
      for (auto *P : Preds) {
        auto It = P->MaterializedValues.find(ID);
        Value *V = (It != P->MaterializedValues.end()) ? It->second
                                                       : VObj.AllocationCall;
        if (!Unique) {
          Unique = V;
        } else if (Unique != V) {
          AllSame = false;
          break;
        }
      }
      // Per-pred materialize: when every pred's MaterializedValue is
      // the OrigAlloc placeholder (the convention used by
      // materializeAtPredFromExitInfo to signal "look me up via
      // MatPerBlock"), each pred actually has its OWN NewInv that the
      // transform resolves through MatPerBlock at apply time. The values
      // are NOT truly unique — they only LOOK unique because they share
      // the placeholder. Build a PHI so the CreatePHI handler can
      // substitute the correct per-pred NewInv into each incoming, and
      // downstream uses thread through the PHI rather than relying on the
      // global RAUW (which is suppressed when IsPerPred=true).
      bool IsPerPredPlaceholder = false;
      if (AllSame && Preds.size() > 1 && Unique == VObj.AllocationCall) {
        AllSame = false;
        IsPerPredPlaceholder = true;
      }
      Value *MV = Unique;
      if (!AllSame) {
        Type *PtrTy =
            PointerType::get(F.getContext(),
                              jeandle::AddrSpace::JavaHeapAddrSpace);
        // Loop-PHI cache: stable PHI across fixpoint iterations.
        PHINode *Phi = getOrCreateLoopFieldPhi(
            BB, ID, /*Offset=*/-1, PtrTy, Preds.size(),
            "pea.materialized.phi");
        jeandle::PEAResult::Effect PE;
        PE.Kind = jeandle::PEAResult::EffectKind::CreatePHI;
        PE.Block = BB;
        // SeqNo is assigned at drain time (end of processBlock(BB))
        // so the CreatePHI sorts strictly after any per-pred Materialize
        // emitted during the same block walk. The placeholder 0 here is
        // overwritten by drainPendingMergePhis.
        PE.SeqNo = 0;
        PE.ObjID = ID;
        PE.PhiInst = Phi;
        PE.PHIType = PtrTy;
        // For the per-pred placeholder case, tell the transform to
        // RAUW OrigAlloc onto the PHI after wiring, so post-merge users
        // resolve through the PHI rather than referencing one of the
        // per-pred NewInvs directly (the IsPerPred Materializes
        // intentionally skip the global RAUW that would otherwise pick a
        // single pred's NewInv and break SSA on other-pred paths).
        PE.RAUWOrigToPHI = IsPerPredPlaceholder;
        for (unsigned i = 0; i < Preds.size(); ++i) {
          auto It = Preds[i]->MaterializedValues.find(ID);
          Value *V = (It != Preds[i]->MaterializedValues.end())
                         ? It->second
                         : VObj.AllocationCall;
          PE.PHIIncomingValues.push_back(V);
          PE.PHIIncomingBlocks.push_back(PredBBs[i]);
        }
        PendingMergePhis[BB].push_back(std::move(PE));
        MV = Phi;
      }
      jeandle::ObjectState OS(0);
      OS.materialize(MV);
      CurrentState.addObject(ID, std::move(OS));
      Materialized.insert(ID);
      continue;
    }

    if (!AllVirtual) {
      // Mixed (virtual on some paths, materialized or missing on others).
      // Short-circuit when the object isn't tracked on every pred (e.g.,
      // an LLVM PHI mixing a virtual incoming from one branch and an
      // unrelated pointer from another). The merge entry simply doesn't
      // carry this ObjectID; processBlockPhis below picks up any LLVM PHI
      // that references it and materializes the virtual incomings.
      bool MissingOnSomePred = false;
      for (auto *P : Preds) {
        if (!P->Materialized.count(ID) && !P->Virtuals.count(ID)) {
          MissingOnSomePred = true;
          break;
        }
      }
      if (MissingOnSomePred) {
        continue;
      }

      // True mixed virtual+materialized at a merge. Inherit Materialized at
      // BB's entry using the OrigAlloc pointer as a placeholder. The transform
      // RAUWs every IR use of OrigAlloc onto the live materialization invoke,
      // so downstream uses of the virtual at the merge resolve correctly
      // without a synthesized ptr addrspace(1) PHI here.
      //
      // "Mixed-merge non-dominating alloc" covers the case
      // where OrigAlloc does NOT dominate BB. For a non-synthetic VO this is
      // unreachable by construction: the only way ID can be tracked at every
      // pred of BB is for OrigAlloc to flow into every pred via SSA, which by
      // dominance requires OrigAlloc to dominate BB. Synthetic VOs (created
      // by Case C at a different merge) escape this invariant — their
      // AllocationCall is borrowed from one of the per-pred source VOs and is
      // not the real backing allocation. Synthetics that hit a downstream
      // mixed-state merge are detected here and dropped to ineligible, which
      // also propagates ineligibility to every per-pred source VO so the
      // original allocations and stores survive in IR. Cascade-materialize
      // is deferred to a follow-on task; the current code keeps the
      // conservative bail.
      jeandle::VirtualObject &VObj = *Result.VirtualObjects[ID];
      if (VObj.IsSynthetic) {
        Eligible[ID] = false;
        for (jeandle::ObjectID PID : VObj.SyntheticSourceIDs)
          Eligible[PID] = false;
        if (VObj.SyntheticPhi)
          Aliases.resetAlias(VObj.SyntheticPhi);
        continue;
      }
      if (!VObj.AllocationCall ||
          !DT.dominates(VObj.AllocationCall, BB)) {
        // "Mixed-merge non-dominating alloc": the proper fix is
        // per-pred materialize + merge-PHI. Until that lands, conservatively
        // mark the VO ineligible so the alloc and per-pred stores survive
        // unmodified in IR. Typed-array-element virtualization can
        // legitimately reach this branch when an array alloc inside a loop
        // body is tracked at the loop exit on every pred (the alloc
        // dominates the loop body but not the post-loop merge).
        Eligible[ID] = false;
        continue;
      }
      jeandle::ObjectState OS(0);
      OS.materialize(VObj.AllocationCall);
      CurrentState.addObject(ID, std::move(OS));
      Materialized.insert(ID);
      continue;
    }

    // All preds report Virtual: check lock counts.
    unsigned RefLC = Preds[0]->LockCounts.lookup(ID);
    bool LocksMatch = true;
    for (const auto *P : Preds) {
      if (P->LockCounts.lookup(ID) != RefLC) {
        LocksMatch = false;
        break;
      }
    }
    if (!LocksMatch) {
      // Lock-count cascade at merge. Handle a virtualCount==N +
      // locksEqual==false merge by falling into the "materialize each
      // virtual pred + build a materializedValuePhi" branch. Each per-pred
      // materialise carries the PRED's OWN lock list, and the resulting
      // alloc-commit emits exactly the pred's own monitorenter set —
      // no synthesized enters are added on the lower-count side, so user
      // semantics are preserved (the lower-count path keeps having fewer
      // real enters than the higher-count one).
      //
      // Mechanism: for each pred whose VO is virtual, call
      // materializeAtPredFromExitInfo so the pred's OWN elided enters
      // (if any) get un-elided in place via ReplaceInput effects, and the
      // alloc itself gets materialized at the pred's terminator. A
      // zero-count pred has nothing to un-elide and just emits the
      // Materialize effect. After the loop, every pred's ExitInfo has
      // flipped to Materialized for ID; the outer retry (Changed=true)
      // falls through to the AllMaterialized branch above which synthesizes
      // the ptr addrspace(1) PHI.
      //
      // Per-pred order is deterministic (the SmallVector iteration is
      // PredBBs's order, which is predecessors(BB) which is deterministic
      // by IR layout). materializeAtPredFromExitInfo's own MatInPH dedup
      // and Eligible bail handle the case where a cascade earlier in this
      // loop already materialized ID at some pred.
      for (unsigned i = 0; i < Preds.size(); ++i) {
        if (!Eligible.lookup(ID))
          break;
        uint32_t PreSeqNo = Result.NextSeqNo;
        materializeAtPredFromExitInfo(ID, PredBBs[i], *Preds[i],
                                       /*SkipGlobalRAUW=*/true,
                                       MatReason::Merge);
        if (Result.NextSeqNo != PreSeqNo)
          Changed = true;
      }
      continue;
    }
    // When LockCounts agree we also require the live enter-stacks to be
    // identical so that any later materializeAt undoes a single, well-defined
    // stack. An earlier implementation compared Call identity only and
    // BAILED the VO on any mismatch.
    //
    // Switch the per-element comparison to (Call, BytecodeDepth) when the
    // JDK frontend's depth metadata is present (otherwise the BytecodeDepth
    // field carries the Order proxy value at push time, recovering exactly
    // the Call-only check for lit tests / stale frontends). On mismatch,
    // do NOT mark the VO ineligible: route to the same per-pred
    // materialize path that the LockCounts-differ branch above uses.
    if (RefLC != 0) {
      const auto &RefStack = Preds[0]->LiveLockEnters.lookup(ID);
      bool StacksMatch = true;
      for (const auto *P : Preds) {
        const auto &S = P->LiveLockEnters.lookup(ID);
        if (S.size() != RefStack.size()) {
          StacksMatch = false;
          break;
        }
        for (unsigned i = 0; i < S.size(); ++i) {
          // Compare Call AND BytecodeDepth. Two paths that re-entered
          // the SAME call site but at different bytecode-level depths (e.g.
          // one path entered a wrapping outer synchronized block first) must
          // be treated as distinct stacks, because un-elision at a downstream
          // escape would emit the same set of ReplaceInput effects either way
          // — but a proper per-pred cascade would have produced
          // lock-correct IR.
          if (S[i].Call != RefStack[i].Call ||
              S[i].BytecodeDepth != RefStack[i].BytecodeDepth) {
            StacksMatch = false;
            break;
          }
        }
        if (!StacksMatch)
          break;
      }
      if (!StacksMatch) {
        // Per-pred materialise each (still-virtual) pred for this
        // VO instead of bailing. Mirrors the LockCounts-mismatch branch
        // above. On the next iter (Changed=true) every pred has flipped
        // ID to Materialized and the AllMaterialized branch at the top of
        // the per-VO loop synthesizes the ptr addrspace(1) PHI.
        for (unsigned i = 0; i < Preds.size(); ++i) {
          if (!Eligible.lookup(ID))
            break;
          uint32_t PreSeqNo = Result.NextSeqNo;
          materializeAtPredFromExitInfo(ID, PredBBs[i], *Preds[i],
                                         /*SkipGlobalRAUW=*/true,
                                         MatReason::Merge);
          if (Result.NextSeqNo != PreSeqNo)
            Changed = true;
        }
        continue;
      }
    }

    // Check field states at every tracked offset. Identical entries flow
    // straight into Merged; disagreements trigger field-PHI synthesis.
    DenseSet<int64_t> Offsets;
    for (const auto *P : Preds) {
      auto FIt = P->FieldStates.find(ID);
      if (FIt == P->FieldStates.end())
        continue;
      for (auto &Kv : FIt->second)
        Offsets.insert(Kv.first);
    }
    SmallVector<int64_t, 8> SortedOffsets(Offsets.begin(), Offsets.end());
    llvm::sort(SortedOffsets); // determinism
    bool BailObject = false;
    DenseMap<int64_t, jeandle::FieldValue> Merged;
    // Snapshot of newly-emitted CreatePHI effects for this object's fields;
    // committed to Result only if every offset succeeds.
    SmallVector<jeandle::PEAResult::Effect, 4> PendingPhiEffects;
    for (int64_t Off : SortedOffsets) {
      // Per-offset bail. When a single offset cannot be merged
      // (incompatible non-integer types, no safe coercion), DROP just
      // that offset from the merged FieldStates rather than marking the
      // entire VO ineligible. Downstream loads from a missing offset
      // see Unknown — tier2Load forces materialization at the load
      // site, which is the conservative-but-sound outcome. Other
      // offsets keep their virtual state. The whole-VO BailObject path
      // remains for unrecoverable conditions (type discovery returned
      // null, LocalBail on inner materialise of an ineligible VO, etc.).
      bool BailOffset = false;
      jeandle::FieldValue Ref = jeandle::FieldValue::unknown();
      bool HaveRef = false;
      bool Disagrees = false;
      for (const auto *P : Preds) {
        jeandle::FieldValue FV = jeandle::FieldValue::unknown();
        auto FIt = P->FieldStates.find(ID);
        if (FIt != P->FieldStates.end()) {
          auto OIt = FIt->second.find(Off);
          if (OIt != FIt->second.end())
            FV = OIt->second;
        }
        if (!HaveRef) {
          Ref = FV;
          HaveRef = true;
        } else if (!Ref.shallowEquals(FV)) {
          Disagrees = true;
          break;
        }
      }
      if (!Disagrees) {
        if (!Ref.isUnknown())
          Merged[Off] = Ref;
        continue;
      }

      // Field disagreement at Off — attempt field-PHI synthesis. Decide
      // the merged PHI type. Compatibility rule: every non-unknown
      // entry's declared type must be identical, OR every non-unknown
      // entry must be a pointer in the Java heap addrspace (in which
      // case the PHI is ptr addrspace(1) and ref/scalar are
      // interchangeable). Additionally allows integer-width
      // promotion: if every non-unknown entry is an integer in the
      // same addrspace (none = primitive), promote the PHI type to the
      // widest integer and zext narrower entries.
      Type *PhiType = nullptr;
      bool AllPointer = true;
      bool AllInteger = true;
      unsigned WidestIntBits = 0;
      for (const auto *P : Preds) {
        auto FIt = P->FieldStates.find(ID);
        if (FIt == P->FieldStates.end())
          continue;
        auto OIt = FIt->second.find(Off);
        if (OIt == FIt->second.end())
          continue;
        const jeandle::FieldValue &FV = OIt->second;
        if (FV.isUnknown())
          continue;
        Type *T = nullptr;
        if (FV.isScalar())
          T = FV.getScalar()->getType();
        else
          T = FV.getDeclaredType();
        if (!T) {
          BailObject = true;
          break;
        }
        if (!T->isPointerTy() ||
            T->getPointerAddressSpace() !=
                jeandle::AddrSpace::JavaHeapAddrSpace)
          AllPointer = false;
        if (T->isIntegerTy()) {
          unsigned B = T->getIntegerBitWidth();
          if (B > WidestIntBits)
            WidestIntBits = B;
        } else {
          AllInteger = false;
        }
        if (!PhiType) {
          PhiType = T;
        } else if (PhiType != T) {
          // Heap-pointer promotion is the existing rule.
          bool BothJavaHeapPtr =
              PhiType->isPointerTy() && T->isPointerTy() &&
              PhiType->getPointerAddressSpace() ==
                  jeandle::AddrSpace::JavaHeapAddrSpace &&
              T->getPointerAddressSpace() ==
                  jeandle::AddrSpace::JavaHeapAddrSpace;
          // Integer-widening promotion: both integers, possibly
          // different widths — defer PhiType selection to the widest
          // and emit zext on narrower per-pred inputs in the value
          // computation pass below.
          bool BothInteger = PhiType->isIntegerTy() && T->isIntegerTy();
          if (!BothJavaHeapPtr && !BothInteger) {
            BailOffset = true;
            break;
          }
          // For integer case, leave PhiType as the LARGER type (we'll
          // re-pick from WidestIntBits below).
        }
      }
      if (BailObject)
        break;
      if (BailOffset) {
        // Drop this offset and keep going.
        continue;
      }
      if (!PhiType) {
        // Should be unreachable — a disagreement implies at least two
        // distinct non-unknown entries.
        continue;
      }
      if (AllPointer) {
        PhiType = PointerType::get(F.getContext(),
                                    jeandle::AddrSpace::JavaHeapAddrSpace);
      } else if (AllInteger && WidestIntBits != 0) {
        // Select the widest integer type as the PHI type and
        // emit zext on narrower per-pred values below.
        PhiType = IntegerType::get(F.getContext(), WidestIntBits);
      }

      // Compute per-pred input value.
      SmallVector<Value *, 4> InValues;
      InValues.reserve(Preds.size());
      bool LocalBail = false;
      for (unsigned i = 0; i < Preds.size(); ++i) {
        jeandle::FieldValue FV = jeandle::FieldValue::unknown();
        auto FIt = Preds[i]->FieldStates.find(ID);
        if (FIt != Preds[i]->FieldStates.end()) {
          auto OIt = FIt->second.find(Off);
          if (OIt != FIt->second.end())
            FV = OIt->second;
        }
        Value *In = nullptr;
        if (FV.isUnknown()) {
          In = jeandle::FieldValue::defaultFor(PhiType);
        } else if (FV.isScalar()) {
          Value *V = FV.getScalar();
          if (V->getType() != PhiType) {
            // Integer-widen via zext if both are integers AND
            // V is strictly narrower than PhiType. Other mismatches
            // (scalar at pointer slot, etc.) still LocalBail.
            if (V->getType()->isIntegerTy() && PhiType->isIntegerTy() &&
                V->getType()->getIntegerBitWidth() <
                    PhiType->getIntegerBitWidth()) {
              // Constant zext folds; non-constant zext synthesised
              // as an unparented OwnedInst the transform splices in
              // before each pred's terminator. Constants are the
              // overwhelming common case (loop-invariant 0/1 stores)
              // so we cover that path here and skip non-const for
              // now (would need a per-pred CreatePHI shim, more
              // plumbing than currently warranted).
              if (auto *CI = dyn_cast<ConstantInt>(V)) {
                In = ConstantInt::get(PhiType,
                                       CI->getValue().zext(
                                           PhiType->getIntegerBitWidth()));
              } else {
                LocalBail = true;
                break;
              }
            } else {
              // Scalar with a non-matching primitive type at a pointer
              // slot, or non-integer width mismatch, or a value-wider-
              // than-PhiType (would be unsound to truncate).
              LocalBail = true;
              break;
            }
          } else {
            In = V;
          }
        } else if (FV.isMaterializedRef()) {
          if (!PhiType->isPointerTy()) {
            LocalBail = true;
            break;
          }
          In = FV.getMaterialized();
        } else if (FV.isVirtualRef()) {
          if (!PhiType->isPointerTy()) {
            LocalBail = true;
            break;
          }
          jeandle::ObjectID InnerID = FV.getVirtualRef();
          // Materialize the inner object at this pred's terminator. After
          // this, the field's effective input value is OrigAlloc(inner) —
          // the transform's MatPerBlock substitutes it with NewInv at apply.
          //
          // Track whether this call emitted any Effects (i.e. did real
          // work, vs short-circuiting on MaterializedAtPred dedup or on a
          // synthetic-VO bail that already happened). The SeqNo delta is a
          // robust proxy: every Effect appended bumps NextSeqNo, and no
          // other code path in materializeAtPredFromExitInfo bumps it.
          uint32_t PreSeqNo = Result.NextSeqNo;
          materializeAtPredFromExitInfo(InnerID, PredBBs[i], *Preds[i],
                                        /*SkipGlobalRAUW=*/false,
                                        MatReason::PHI);
          if (Result.NextSeqNo != PreSeqNo)
            Changed = true;
          if (!Eligible.lookup(InnerID)) {
            LocalBail = true;
            break;
          }
          jeandle::VirtualObject &InnerVO =
              *Result.VirtualObjects[InnerID];
          // Defensively rewrite this pred's outer-VO FieldStates
          // entry for the just-materialized inner to MaterializedRef
          // so a sibling successor of the pred (other-than-BB) that
          // later inherits from Preds[i] sees the materialized pointer
          // rather than a stale VirtualRef(InnerID).
          // updateOtherStatesForMaterialized inside materializeAtPred-
          // FromExitInfo already covers this for ALL sibling VOs,
          // but the defensive write here doubles as a guard against
          // any future refactor that removes the sibling sweep.
          Preds[i]->FieldStates[ID][Off] =
              jeandle::FieldValue::materializedRef(
                  InnerVO.AllocationCall);
          In = InnerVO.AllocationCall;
        } else {
          LocalBail = true;
          break;
        }
        InValues.push_back(In);
      }
      if (LocalBail) {
        // Per-offset bail. Drop this offset, keep going.
        continue;
      }

      // Loop-PHI cache: stable PHI across fixpoint iterations.
      PHINode *Phi = getOrCreateLoopFieldPhi(BB, ID, Off, PhiType,
                                              Preds.size(), "pea.field.phi");
      jeandle::PEAResult::Effect PE;
      PE.Kind = jeandle::PEAResult::EffectKind::CreatePHI;
      PE.Block = BB;
      // SeqNo assigned at drain time; see PendingMergePhis comment.
      PE.SeqNo = 0;
      PE.ObjID = ID;
      PE.PhiInst = Phi;
      PE.PHIType = PhiType;
      for (unsigned i = 0; i < Preds.size(); ++i) {
        PE.PHIIncomingValues.push_back(InValues[i]);
        PE.PHIIncomingBlocks.push_back(PredBBs[i]);
      }
      PendingPhiEffects.push_back(std::move(PE));

      if (PhiType->isPointerTy())
        Merged[Off] = jeandle::FieldValue::materializedRef(Phi);
      else
        Merged[Off] = jeandle::FieldValue::scalar(Phi);
    }
    if (BailObject) {
      Eligible[ID] = false;
      continue;
    }

    // Defer pending field-PHI effects to PendingMergePhis so the
    // drain at end of processBlock(BB) reassigns their SeqNos AFTER any
    // per-pred Materialize emitted during the block walk.
    for (auto &PE : PendingPhiEffects)
      PendingMergePhis[BB].push_back(std::move(PE));

    // Case B: object stays virtual at BB entry with the merged field state.
    CurrentState.addObject(ID, jeandle::ObjectState(/*numEntries=*/0));
    if (!Merged.empty())
      FieldStates[ID] = std::move(Merged);
    if (RefLC != 0) {
      LockCounts[ID] = RefLC;
      // The merged live stack is identical to (any) pred's stack, since the
      // StacksMatch check above succeeded.
      const auto &RefStack = Preds[0]->LiveLockEnters.lookup(ID);
      if (!RefStack.empty())
        LiveLockEnters[ID] = RefStack;
    }
    }

    // Run the PHI loop INSIDE the merge do-while. Case-A fallback
    // and Case-C synthesis both call materializeAtPredFromExitInfo on
    // inner / per-pred VOs; any such call mutates a pred's ExitInfo
    // (Virtuals->Materialized for the affected ID), which can invalidate
    // the per-VO decisions just made. We detect the work via Result
    // .NextSeqNo delta (every emitted Effect bumps it) and set Changed=true
    // so the next iter re-runs the per-VO loop against the updated pred
    // ExitInfos. processBlockPhis only modifies AliasMap (Case-B) and per-
    // pred BlockEffects / ExitInfos (Case-A/C); the merge-block-local
    // CurrentState / FieldStates / LockCounts / OwnedPhis it touches are
    // all snapshotted in MergeSnapshot so a rollback before iter N+1 is
    // sound. The CaseCVOCache / LoopFieldPhiCache survive the rollback
    // by design (loop-header stability across iterations).
    {
      uint32_t PrePhiSeqNo = Result.NextSeqNo;
      processBlockPhis(BB);
      if (Result.NextSeqNo != PrePhiSeqNo)
        Changed = true;
    }
  } while (Changed);
}

void Analyzer::snapshotExitStateInto(BlockExitData &Data) {
  for (auto &VObjUP : Result.VirtualObjects) {
    jeandle::ObjectID ID = VObjUP->getID();
    if (!Eligible.lookup(ID))
      continue;
    const jeandle::ObjectState *OS = CurrentState.getObjectStateOptional(ID);
    if (!OS)
      continue;
    if (OS->isVirtual()) {
      Data.Virtuals.insert(ID);
      auto FIt = FieldStates.find(ID);
      if (FIt != FieldStates.end() && !FIt->second.empty())
        Data.FieldStates[ID] = FIt->second;
      auto LIt = LockCounts.find(ID);
      if (LIt != LockCounts.end() && LIt->second != 0)
        Data.LockCounts[ID] = LIt->second;
      // Snapshot the live monitorenter stack so successor blocks see the
      // same path-specific call set we did.
      auto SIt = LiveLockEnters.find(ID);
      if (SIt != LiveLockEnters.end() && !SIt->second.empty())
        Data.LiveLockEnters[ID] = SIt->second;
    } else if (OS->isMaterialized()) {
      Data.Materialized.insert(ID);
      // Capture the materialized pointer at block exit so downstream
      // merges can use it as the PHI input on this edge.
      Data.MaterializedValues[ID] = OS->getMaterializedValue();
    }
  }
}

void Analyzer::snapshotExitState(BasicBlock *BB) {
  BlockExitInfo Info;
  snapshotExitStateInto(Info);
  BlockExits[BB] = std::move(Info);
}

void Analyzer::processBlockPhis(BasicBlock *BB) {
  // Walk explicit LLVM PHIs of java-heap pointers. Other PHIs (e.g., scalar
  // i32 PHIs of folded virtual-load results) flow through normal SSA and are
  // not the concern of the analyzer.
  for (PHINode &Phi : BB->phis()) {
    Type *Ty = Phi.getType();
    if (!Ty->isPointerTy())
      continue;
    if (cast<PointerType>(Ty)->getAddressSpace() !=
        jeandle::AddrSpace::JavaHeapAddrSpace)
      continue;

    // Resolve each incoming against its predecessor's exit snapshot. We only
    // check the AliasMap (functions like full resolveVirtualRef would need a
    // pred-specific PEABlockState, which we don't have on hand); pointer
    // PHIs whose incomings are themselves derived through GEP/cast chains
    // are uncommon enough that the alias-only resolution is sufficient.
    // Note that the AliasMap is function-wide so a virtual alias registered
    // in any block is visible here.
    SmallVector<std::optional<jeandle::ObjectID>, 4> InIDs;
    bool AnyVirtual = false;
    InIDs.reserve(Phi.getNumIncomingValues());
    for (unsigned I = 0; I < Phi.getNumIncomingValues(); ++I) {
      BasicBlock *Pred = Phi.getIncomingBlock(I);
      Value *V = Phi.getIncomingValue(I);
      std::optional<jeandle::ObjectID> Found;
      auto AID = Aliases.getVirtualAlias(V);
      BlockExitData *PredED = exitDataFor(Pred, BB);
      if (AID && PredED && PredED->Virtuals.count(*AID))
        Found = *AID;
      InIDs.push_back(Found);
      if (Found)
        AnyVirtual = true;
    }
    if (!AnyVirtual)
      continue;

    // Case B: every incoming agrees on the same ObjectID AND the object is
    // still virtual at merge entry (mergeStates kept it).
    bool AllSame = true;
    std::optional<jeandle::ObjectID> First;
    for (auto &O : InIDs) {
      if (!O) {
        AllSame = false;
        break;
      }
      if (!First)
        First = O;
      else if (*First != *O) {
        AllSame = false;
        break;
      }
    }

    if (AllSame && First) {
      const jeandle::ObjectState *OS =
          CurrentState.getObjectStateOptional(*First);
      if (OS && OS->isVirtual()) {
        // Register the PHI as an alias for the same ObjectID so downstream
        // load/store handlers in this block resolve through it.
        Aliases.addVirtualAlias(&Phi, *First);
        // Also record the PHI on the per-VO Case-B alias list so
        // commit() can hand the transform an erase request if the VO
        // ends up NeverEscapes. The PHI's incomings will all be the
        // VO's OrigAlloc (the analyzer never rewrites Case-B PHI
        // operands), which Pass 2's EliminateAllocation RAUWs to poison;
        // the PHI is then trivially dead and we erase it explicitly so
        // the IR doesn't carry a `phi [poison, poison]` artefact past
        // PEA.
        CaseBPhiAliases[*First].push_back(&Phi);
        continue;
      }
    }

    // Case C: every incoming resolves to a virtual ID, but the IDs are not
    // all equal. Attempt to synthesize a single merged VirtualObject. On
    // success the PHI is aliased to the new VO and downstream uses fold
    // through it. On failure (compatibility, identity, or per-entry type
    // checks) we fall through to Case A.
    bool TryCaseC = (First /* at least one virtual */) &&
                    !AllSame; // Case B already returned if AllSame succeeded.
    if (TryCaseC) {
      bool EveryInputVirtual = true;
      for (auto &O : InIDs) {
        if (!O) {
          EveryInputVirtual = false;
          break;
        }
      }
      if (EveryInputVirtual && synthesizeCaseC(BB, &Phi, InIDs))
        continue;
    }

    // Case A: mixed virtual + non-virtual incomings, OR a Case C attempt
    // that bailed. For every virtual incoming, materialize at that
    // incoming's predecessor terminator. The PHI itself stays in IR;
    // transform-time RAUW (and MatPerBlock substitution at PHI nodes we
    // synthesize elsewhere) handles operand updates.
    for (unsigned I = 0; I < Phi.getNumIncomingValues(); ++I) {
      if (!InIDs[I])
        continue;
      BasicBlock *Pred = Phi.getIncomingBlock(I);
      BlockExitData *PredED = exitDataFor(Pred, BB);
      if (!PredED) {
        Eligible[*InIDs[I]] = false;
        continue;
      }
      materializeAtPredFromExitInfo(*InIDs[I], Pred, *PredED,
                                    /*SkipGlobalRAUW=*/false, MatReason::PHI);
    }
  }
}

bool Analyzer::synthesizeCaseC(
    BasicBlock *BB, PHINode *Phi,
    ArrayRef<std::optional<jeandle::ObjectID>> InIDs) {
  const unsigned N = Phi->getNumIncomingValues();
  assert(InIDs.size() == N);

  // Resolve per-pred VO ids, predecessor blocks, and their exit snapshots.
  SmallVector<jeandle::ObjectID, 4> PerPredIDs;
  PerPredIDs.reserve(N);
  SmallVector<BasicBlock *, 4> Preds;
  Preds.reserve(N);
  SmallVector<BlockExitData *, 4> ExitInfos;
  ExitInfos.reserve(N);
  for (unsigned i = 0; i < N; ++i) {
    if (!InIDs[i])
      return false;
    PerPredIDs.push_back(*InIDs[i]);
    BasicBlock *P = Phi->getIncomingBlock(i);
    Preds.push_back(P);
    // Respect state-split for unwind successors. exitDataFor returns
    // nullptr if the pred's invoke was virtualized (unwind edge dead), in
    // which case Case C cannot synthesize a merged VO that depends on
    // that pred's data — bail.
    BlockExitData *ED = exitDataFor(P, BB);
    if (!ED)
      return false;
    ExitInfos.push_back(ED);
    // Each per-pred VO must be eligible AND still virtual at pred exit.
    if (!Eligible.lookup(PerPredIDs.back()))
      return false;
    if (!ExitInfos.back()->Virtuals.count(PerPredIDs.back()))
      return false;
  }

  // Compatibility check.
  jeandle::VirtualObject &Ref = *Result.VirtualObjects[PerPredIDs[0]];
  for (unsigned i = 1; i < N; ++i) {
    jeandle::VirtualObject &VO = *Result.VirtualObjects[PerPredIDs[i]];
    if (VO.getKind() != Ref.getKind())
      return false;
    if (VO.Klass != Ref.Klass)
      return false;
    if (Ref.isArray()) {
      if (VO.ArrayElementType != Ref.ArrayElementType)
        return false;
      if (VO.ArrayLength != Ref.ArrayLength)
        return false;
      if (VO.ArrayIndexScale != Ref.ArrayIndexScale)
        return false;
      if (VO.ArrayBaseOffset != Ref.ArrayBaseOffset)
        return false;
    }
    if (VO.entryCount() != Ref.entryCount())
      return false;
  }

  // Lock compatibility (locksEqual).
  unsigned RefLC = ExitInfos[0]->LockCounts.lookup(PerPredIDs[0]);
  for (unsigned i = 1; i < N; ++i) {
    if (ExitInfos[i]->LockCounts.lookup(PerPredIDs[i]) != RefLC)
      return false;
  }
  if (RefLC != 0) {
    const auto &RefStack =
        ExitInfos[0]->LiveLockEnters.lookup(PerPredIDs[0]);
    for (unsigned i = 1; i < N; ++i) {
      const auto &S = ExitInfos[i]->LiveLockEnters.lookup(PerPredIDs[i]);
      if (S.size() != RefStack.size())
        return false;
      // Compare Call AND BytecodeDepth so that two stacks built
      // from the same call sites but at different bytecode depths are not
      // collapsed by the Case-C identity-merge fast path. The legacy Order
      // tag is still ignored here (the loop fixpoint refreshes Order on
      // every re-push, which would diverge across iterations).
      for (unsigned k = 0; k < S.size(); ++k)
        if (S[k].Call != RefStack[k].Call ||
            S[k].BytecodeDepth != RefStack[k].BytecodeDepth)
          return false;
    }
  }

  // Boxed-primitive fast path: when every per-pred VO is a same-kind boxed primitive
  // wrapper (Boolean/Byte/.../Double), drop the identity check entirely.
  // A boxed-primitive virtual has no identity, so Case-C-style merges of
  // two boxed allocations are permitted regardless of external users —
  // the merged virtual is treated as a structural value carrier, not an
  // identity carrier. We key on the BoxedPrimitiveKind tag installed by
  // tier1Allocate. Note: this gate is currently dormant in production
  // because the JDK frontend does not yet lower autoboxing to
  // `jeandle.new_instance + store`; the tag stays at the
  // JBasicType::Count sentinel for non-boxed VOs so the path below
  // remains the conservative default. Lit tests synthesize the boxed
  // pattern by hand and rely on this fast-path.
  bool AllBoxedSameKind = true;
  {
    jeandle::VirtualObject &Ref0 = *Result.VirtualObjects[PerPredIDs[0]];
    if (!Ref0.isBoxedPrimitive())
      AllBoxedSameKind = false;
    else {
      for (unsigned i = 1; i < N; ++i) {
        jeandle::VirtualObject &VOi = *Result.VirtualObjects[PerPredIDs[i]];
        if (!VOi.isBoxedPrimitive() ||
            VOi.BoxedPrimitiveKind != Ref0.BoxedPrimitiveKind) {
          AllBoxedSameKind = false;
          break;
        }
      }
    }
  }

  // Early-bail if any incoming of Phi equals Phi itself (a
  // back-edge self-reference on a loop-header PHI). A self-loop incoming
  // means the per-pred VO this slot resolves to is one that we're about
  // to synthesise *into* — there is no per-pred independent allocation
  // and the identity check cannot meaningfully proceed.
  for (unsigned i = 0; i < N; ++i) {
    if (Phi->getIncomingValue(i) == Phi)
      return false;
  }

  // Identity check (single-usage-allocation). Every non-boxed VO has
  // identity. For each per-pred VO we require:
  //   (a) the LLVM PHI is the only "external" user of the per-pred alloc.
  //       An "external" user is one that is neither (i) covered by a planned
  //       PEA effect for that ID (EliminateStore, ReplaceLoad, ReplaceCall,
  //       EliminateAllocation, Materialize), nor (ii) registered in the
  //       AliasMap as a virtual alias of the same ID (GEP/cast/freeze
  //       forwarded by propagatePointerAlias).
  //   (b) no OTHER VO at any pred references this VO via virtualRef in its
  //       FieldStates (otherwise materializing that other VO would also
  //       materialize this one and expose identity).
  if (!AllBoxedSameKind) {
    for (unsigned i = 0; i < N; ++i) {
      jeandle::ObjectID PID = PerPredIDs[i];
      jeandle::VirtualObject &PVO = *Result.VirtualObjects[PID];
      CallBase *OrigAlloc = PVO.AllocationCall;
      if (!OrigAlloc)
        return false;
      // Cheap-out — an alloc with only one use (the PHI itself)
      // trivially satisfies the no-other-external-user requirement, so
      // the expensive Effect-target collection below is unnecessary.
      // hasNUsesOrMore(2) does an O(1) check against the use list.
      bool MaybeOtherUsers = OrigAlloc->hasNUsesOrMore(2);
      if (MaybeOtherUsers) {
        DenseSet<Instruction *> InternalTargets;
        for (auto &Kv : Result.BlockEffects) {
          for (const auto &E : Kv.second) {
            if (E.ObjID == PID && E.Target)
              InternalTargets.insert(E.Target);
          }
        }
        for (User *U : OrigAlloc->users()) {
          if (U == Phi)
            continue;
          auto *UI = dyn_cast<Instruction>(U);
          if (!UI)
            return false;
          if (InternalTargets.count(UI))
            continue;
          auto AID = Aliases.getVirtualAlias(UI);
          if (AID && *AID == PID)
            continue;
          // An unaccounted-for user — identity-bail.
          return false;
        }
      }
      // No other VO references PID through a virtualRef field entry.
      // Cross-check both per-pred ExitInfos AND the analyzer's
      // live FieldStates / CurrentState. A VO synthesized earlier in
      // this same mergeStates iteration (e.g. a prior Case-C inner)
      // may carry a VirtualRef(PID) entry that isn't yet reflected in
      // any pred's ExitInfo — the analyzer's CurrentState is the only
      // place it lives until snapshotExitState runs. Missing this
      // check leaks identity through the in-flight synthesis.
      for (auto *EI : ExitInfos) {
        for (auto &Kv : EI->FieldStates) {
          if (Kv.first == PID)
            continue;
          for (auto &Off : Kv.second) {
            if (Off.second.isVirtualRef() &&
                Off.second.getVirtualRef() == PID)
              return false;
          }
        }
      }
      for (auto &Kv : FieldStates) {
        if (Kv.first == PID)
          continue;
        for (auto &Off : Kv.second) {
          if (Off.second.isVirtualRef() &&
              Off.second.getVirtualRef() == PID)
            return false;
        }
      }
    }
  }

  // Loop-header cache lookup (forward infra; misses under single-pass).
  CaseCKey CacheKey;
  CacheKey.Block = BB;
  {
    SmallVector<jeandle::ObjectID, 4> Sorted(PerPredIDs.begin(),
                                              PerPredIDs.end());
    llvm::sort(Sorted);
    CacheKey.SourceIDs = std::move(Sorted);
  }
  // Peek the cache for an already-synthesised VO at this loop header.
  // We previously returned EARLY on a cache hit (just re-aliasing the PHI and
  // re-adding an empty ObjectState), which left FieldStates[Cached] EMPTY on
  // every iter >= 1 — any field load against the synthetic VO inside the loop
  // body would then see "unknown" and force materialization. Instead, fall
  // through into the full synthesize path, using `CachedExistingID` as the
  // reused ObjectID; the PHI emission code below uses getOrCreateLoopFieldPhi
  // so the per-offset PHI shells (and thus FieldStates' Value*) are stable
  // across iterations.
  bool IsLoopHeader = LI.isLoopHeader(BB);
  jeandle::ObjectID CachedExistingID = jeandle::InvalidObjectID;
  if (IsLoopHeader) {
    auto CIt = CaseCVOCache.find(CacheKey);
    if (CIt != CaseCVOCache.end()) {
      jeandle::ObjectID Cached = CIt->second;
      if (Eligible.lookup(Cached))
        CachedExistingID = Cached;
      // If the cached ID was made ineligible (e.g. materialized in a previous
      // top-level pass), fall through and synthesize a fresh VO.
    }
  }

  // Pre-flight: compute per-entry merged type and per-pred input values for
  // every offset in the union of per-pred FieldStates. We do this BEFORE
  // creating the new VO so a type-mismatch bail leaves Result.VirtualObjects
  // unchanged.
  DenseSet<int64_t> OffsetsSet;
  for (unsigned i = 0; i < N; ++i) {
    auto FIt = ExitInfos[i]->FieldStates.find(PerPredIDs[i]);
    if (FIt == ExitInfos[i]->FieldStates.end())
      continue;
    for (auto &Kv : FIt->second)
      OffsetsSet.insert(Kv.first);
  }
  SmallVector<int64_t, 8> Offsets(OffsetsSet.begin(), OffsetsSet.end());
  llvm::sort(Offsets);

  struct OffsetPlan {
    int64_t Off;
    Type *PhiType = nullptr;
    bool AllSame = true;
    jeandle::FieldValue SoleValue;
    SmallVector<jeandle::FieldValue, 4> PerPredFVs;
  };
  SmallVector<OffsetPlan, 8> Plans;
  Plans.reserve(Offsets.size());

  for (int64_t Off : Offsets) {
    OffsetPlan P;
    P.Off = Off;
    P.PerPredFVs.resize(N);
    Type *PhiType = nullptr;
    bool AllPointer = true;
    for (unsigned i = 0; i < N; ++i) {
      jeandle::FieldValue FV = jeandle::FieldValue::unknown();
      auto FIt = ExitInfos[i]->FieldStates.find(PerPredIDs[i]);
      if (FIt != ExitInfos[i]->FieldStates.end()) {
        auto OIt = FIt->second.find(Off);
        if (OIt != FIt->second.end())
          FV = OIt->second;
      }
      P.PerPredFVs[i] = FV;
      if (FV.isUnknown())
        continue;
      Type *T = FV.isScalar() ? FV.getScalar()->getType() : FV.getDeclaredType();
      if (!T)
        return false;
      if (!T->isPointerTy() ||
          T->getPointerAddressSpace() !=
              jeandle::AddrSpace::JavaHeapAddrSpace)
        AllPointer = false;
      if (!PhiType)
        PhiType = T;
      else if (PhiType != T &&
               !(PhiType->isPointerTy() && T->isPointerTy() &&
                 PhiType->getPointerAddressSpace() ==
                     jeandle::AddrSpace::JavaHeapAddrSpace &&
                 T->getPointerAddressSpace() ==
                     jeandle::AddrSpace::JavaHeapAddrSpace))
        return false;
    }
    if (!PhiType)
      continue; // every pred has this slot unknown — nothing to record.
    if (AllPointer)
      PhiType = PointerType::get(F.getContext(),
                                  jeandle::AddrSpace::JavaHeapAddrSpace);
    P.PhiType = PhiType;
    // Detect AllSame across preds (shallowEquals comparison).
    jeandle::FieldValue First = P.PerPredFVs[0];
    P.SoleValue = First;
    bool Same = true;
    for (unsigned i = 1; i < N; ++i)
      if (!First.shallowEquals(P.PerPredFVs[i])) {
        Same = false;
        break;
      }
    P.AllSame = Same && !First.isUnknown();
    Plans.push_back(std::move(P));
  }

  // Synthesize (or REUSE on cache hit) the new VirtualObject. On a
  // cache hit we reuse the existing ID (and existing VirtualObjects slot)
  // and merely refresh its synthetic metadata. On a miss we duplicate Ref,
  // allocate a fresh ID via createVirtualObject, and tag it synthetic.
  jeandle::ObjectID NewID;
  if (CachedExistingID != jeandle::InvalidObjectID) {
    NewID = CachedExistingID;
    jeandle::VirtualObject &VO = *Result.VirtualObjects[NewID];
    VO.IsSynthetic = true;
    VO.SyntheticSourceIDs.assign(PerPredIDs.begin(), PerPredIDs.end());
    VO.SyntheticPhi = Phi;
  } else {
    auto NewVOUP = Ref.duplicate();
    NewID = Result.createVirtualObject(std::move(NewVOUP));
    jeandle::VirtualObject &NewVO = *Result.VirtualObjects[NewID];
    NewVO.IsSynthetic = true;
    NewVO.SyntheticSourceIDs.assign(PerPredIDs.begin(), PerPredIDs.end());
    NewVO.SyntheticPhi = Phi;
  }
  // Note: NewVO.AllocationCall is shared with Ref (the first per-pred VO).
  // It MUST NOT be used as a Materialize target or for RAUW — the synthetic
  // guard in materializeAt prevents that. We keep the field non-null only
  // because some accessors don't tolerate null AllocationCall (no current
  // path reaches them for a synthetic VO).

  // Materialize inner virtuals if any per-pred entry is a VirtualRef. This
  // happens BEFORE we emit CreatePHI effects so the PHI inputs point at the
  // inner VO's original allocation (which the transform later RAUWs onto the
  // materialized invoke via NewAllocFor). Any failure here marks the new VO
  // ineligible and returns false; the per-entry CreatePHI effects we add
  // below (for NewID) get dropped at commit. Inner materializations may have
  // side-effects on snapshot state, but those are independently sound.
  DenseMap<int64_t, jeandle::FieldValue> Merged;
  SmallVector<jeandle::PEAResult::Effect, 4> PendingPhiEffects;

  for (const OffsetPlan &P : Plans) {
    if (P.AllSame) {
      Merged[P.Off] = P.SoleValue;
      continue;
    }
    // Compute per-pred Value* for the synthesized PHI.
    SmallVector<Value *, 4> InValues;
    InValues.reserve(N);
    for (unsigned i = 0; i < N; ++i) {
      const jeandle::FieldValue &FV = P.PerPredFVs[i];
      Value *In = nullptr;
      if (FV.isUnknown()) {
        In = jeandle::FieldValue::defaultFor(P.PhiType);
      } else if (FV.isScalar()) {
        Value *V = FV.getScalar();
        if (V->getType() != P.PhiType) {
          Eligible[NewID] = false;
          return false;
        }
        In = V;
      } else if (FV.isMaterializedRef()) {
        if (!P.PhiType->isPointerTy()) {
          Eligible[NewID] = false;
          return false;
        }
        In = FV.getMaterialized();
      } else if (FV.isVirtualRef()) {
        if (!P.PhiType->isPointerTy()) {
          Eligible[NewID] = false;
          return false;
        }
        jeandle::ObjectID InnerID = FV.getVirtualRef();
        materializeAtPredFromExitInfo(InnerID, Preds[i], *ExitInfos[i],
                                      /*SkipGlobalRAUW=*/false,
                                      MatReason::PHI);
        if (!Eligible.lookup(InnerID)) {
          Eligible[NewID] = false;
          return false;
        }
        // Same defensive ExitInfo rewrite as the merge per-VO
        // loop; see the matching comment in mergeStates. The per-pred
        // sibling sweep inside materializeAtPredFromExitInfo already
        // does this for ALL FieldStates entries; the explicit write
        // here guards against future refactors and makes the post-
        // condition local to this synthesise-Case-C call.
        ExitInfos[i]->FieldStates[PerPredIDs[i]][P.Off] =
            jeandle::FieldValue::materializedRef(
                Result.VirtualObjects[InnerID]->AllocationCall);
        In = Result.VirtualObjects[InnerID]->AllocationCall;
      } else {
        Eligible[NewID] = false;
        return false;
      }
      InValues.push_back(In);
    }
    // At a loop-header BB, route through the LoopFieldPhiCache so
    // the per-(BB, NewID, Off) PHI shell is REUSED across loop-fixpoint
    // iterations. Same Value* across iters keeps FieldStates structurally
    // equivalent for the convergence check. For non-header BBs the cache
    // is bypassed (getOrCreateLoopFieldPhi falls back to createUnparentedPhi).
    PHINode *NewPhi = getOrCreateLoopFieldPhi(BB, NewID, P.Off, P.PhiType, N,
                                              "pea.casec.field.phi");
    jeandle::PEAResult::Effect PE;
    PE.Kind = jeandle::PEAResult::EffectKind::CreatePHI;
    PE.Block = BB;
    // SeqNo assigned at drain time; see PendingMergePhis comment.
    PE.SeqNo = 0;
    PE.ObjID = NewID;
    PE.PhiInst = NewPhi;
    PE.PHIType = P.PhiType;
    for (unsigned i = 0; i < N; ++i) {
      PE.PHIIncomingValues.push_back(InValues[i]);
      PE.PHIIncomingBlocks.push_back(Preds[i]);
    }
    PendingPhiEffects.push_back(std::move(PE));
    if (P.PhiType->isPointerTy())
      Merged[P.Off] = jeandle::FieldValue::materializedRef(NewPhi);
    else
      Merged[P.Off] = jeandle::FieldValue::scalar(NewPhi);
  }

  // Defer pending field-PHI effects to PendingMergePhis so the drain
  // at end of processBlock(BB) reassigns their SeqNos AFTER any per-pred
  // Materialize emitted during the block walk. See PendingMergePhis comment.
  for (auto &PE : PendingPhiEffects)
    PendingMergePhis[BB].push_back(std::move(PE));
  CurrentState.addObject(NewID, jeandle::ObjectState(/*numEntries=*/0));
  Eligible[NewID] = true;
  if (!Merged.empty())
    FieldStates[NewID] = std::move(Merged);
  if (RefLC != 0) {
    LockCounts[NewID] = RefLC;
    const auto &RefStack =
        ExitInfos[0]->LiveLockEnters.lookup(PerPredIDs[0]);
    if (!RefStack.empty())
      LiveLockEnters[NewID] = RefStack;
  }
  Aliases.addVirtualAlias(Phi, NewID);
  // Only insert if this is a fresh synthesis. On a cache hit the
  // entry already exists for the same key; emplace would be a no-op but we
  // skip explicitly to avoid the (CacheKey already moved-from) hazard above.
  if (IsLoopHeader && CachedExistingID == jeandle::InvalidObjectID)
    CaseCVOCache.emplace(std::move(CacheKey), NewID);
  return true;
}

void Analyzer::applyThreeTier(Instruction *I) {
  // PHINodes are handled in processBlockPhis (which runs before this loop)
  // and have their alias status (Case B) or per-pred materialization (Case A)
  // recorded there. Re-walking them in the generic three-tier dispatch would
  // hit the hasVirtualInputs fall-through and incorrectly trigger
  // materializeAllVirtualOperands, dropping a successfully-aliased Case-B PHI
  // back to materialized.
  if (isa<PHINode>(I))
    return;

  // Tier 1: Jeandle allocation site.
  if (auto *CB = dyn_cast<CallBase>(I)) {
    if (jeandle::pea::isJeandleAllocation(CB)) {
      tier1Allocate(CB);
      return;
    }
  }

  // Tier 2 store/load — try them regardless of the hasVirtualInputs gate.
  // tier2Store/Load resolve the pointer through GEPs/casts and early-exit if
  // it doesn't bottom out on a virtual base.
  if (auto *SI = dyn_cast<StoreInst>(I)) {
    // tier2Store returns true if the POINTER side was a virtual,
    // i.e. it consumed the store. If false, we MUST fall through to the
    // generic hasVirtualInputs path so a VALUE-side virtual reference
    // (e.g. `store ptr %virtAlloc, ptr @G`) escapes correctly instead of
    // silently surviving in IR as a `store poison, ptr @G` after
    // EliminateAllocation RAUWs the virtual to PoisonValue.
    if (tier2Store(SI))
      return;
    // Fall through.
  } else if (auto *LI = dyn_cast<LoadInst>(I)) {
    tier2Load(LI);
    return;
  }

  // TODO: tier2AtomicRMW / tier2CmpXchg dispatch. Re-enable together with
  // the jeandle-jdk frontend inliner for Unsafe atomic intrinsics. Until
  // then any atomicrmw/cmpxchg falls through to the generic-escape path
  // below, which materializes the virtual conservatively.

  // Other tier-2 / tier-3 consumers.
  if (Aliases.hasVirtualInputs(I)) {
    // Pointer-derivation forwards the virtual alias to the derived pointer so
    // downstream load/store handlers can pick up the base via the alias map.
    // SelectInst is included here: when both arms resolve to
    // the same virtual ObjectID, the Select denotes that virtual on every
    // execution path (resolveVirtualRefImpl recurses through Select arms).
    // If the arms disagree or aren't fully virtual, propagatePointerAlias
    // falls through to materializeAllVirtualOperands, matching the Case-A
    // PHI handling for ambiguous merges.
    if (isa<GetElementPtrInst>(I) || isa<BitCastInst>(I) ||
        isa<AddrSpaceCastInst>(I) || isa<FreezeInst>(I) ||
        isa<SelectInst>(I)) {
      propagatePointerAlias(I);
      return;
    }
    // §2.3.14: known non-escaping LLVM intrinsics (assume, lifetime markers,
    // invariant markers, debug intrinsics, ...) are no-ops for PEA. The
    // virtual stays virtual and the call is left alone in IR (some are
    // DCE'd downstream; others are harmless). Must run BEFORE the JavaOp
    // fold + generic-escape fall-through.
    if (auto *II = dyn_cast<IntrinsicInst>(I)) {
      // Extra debug-only assert — PEA must run BEFORE
      // RewriteStatepointsForGC. A statepoint intrinsic appearing here means
      // the pass scheduling has been broken; bail loudly in debug, fall
      // through to the generic-escape path in release (safe — every
      // statepoint operand is forced to materialize via the
      // hasVirtualInputs handler).
      assert(II->getIntrinsicID() != Intrinsic::experimental_gc_statepoint &&
             II->getIntrinsicID() != Intrinsic::experimental_gc_relocate &&
             II->getIntrinsicID() != Intrinsic::experimental_gc_result &&
             "PEA must not run after RewriteStatepointsForGC");
      switch (II->getIntrinsicID()) {
      case Intrinsic::assume:
      case Intrinsic::lifetime_start:
      case Intrinsic::lifetime_end:
      case Intrinsic::invariant_start:
      case Intrinsic::invariant_end:
      case Intrinsic::experimental_noalias_scope_decl:
      case Intrinsic::dbg_declare:
      case Intrinsic::dbg_value:
      case Intrinsic::dbg_label:
      case Intrinsic::donothing:
      case Intrinsic::sideeffect:
      // Extended allowlist. None of these intrinsics produce a
      // pointer with a different identity than their argument, none mutate
      // memory we care about, and none cross the heap/abstract boundary —
      // all safe to leave in IR alongside a virtual without forcing escape.
      // ptr.annotation/var.annotation: TBAA-style debug annotation. The
      //   call returns nothing meaningful and its operand is purely
      //   informational.
      // is.constant / expect / expect.with.probability: branch-prediction
      //   hints; their value-result is i1/iN derived from a primitive
      //   (the predicate or the comparison value), not from the virtual
      //   pointer's identity, so the virtual doesn't escape through them.
      // allow.runtime.check / allow.ubsan.check: similar — return i1.
      case Intrinsic::ptr_annotation:
      case Intrinsic::var_annotation:
      case Intrinsic::is_constant:
      case Intrinsic::expect:
      case Intrinsic::expect_with_probability:
      case Intrinsic::allow_runtime_check:
      case Intrinsic::allow_ubsan_check:
        return;
      // launder/strip invariant.group are pointer-identity-
      // preserving. resolveVirtualRef does not recurse through CallInst, so
      // propagatePointerAlias would fall through to
      // materializeAllVirtualOperands. Directly forward the argument's
      // virtual alias to the result instead.
      case Intrinsic::launder_invariant_group:
      case Intrinsic::strip_invariant_group: {
        Value *Arg = II->getArgOperand(0);
        if (auto BaseID =
                jeandle::pea::resolveVirtualRef(Arg, CurrentState,
                                                Aliases, DL)) {
          Aliases.addVirtualAlias(II, *BaseID);
          return;
        }
        // Argument doesn't resolve to a virtual: not virtual-relevant; the
        // call has no PEA effect, so leave it alone (no escape).
        return;
      }
      default:
        break;
      }
    }
    // TODO: tier2ArrayCopy / tier2MemSet dispatch. Re-enable together with
    // a jeandle-jdk frontend inliner that emits llvm.memcpy/memmove
    // (System.arraycopy) or llvm.memset (Arrays.fill / user-array zero-init)
    // before PEA runs. Today neither shape reaches PEA — see
    // jeandle-llvm/llvm/lib/Jeandle/Pipeline.cpp; the only llvm.memset
    // producer lives inside jeandle.new_instance's lower-phase=1 template,
    // which is inlined after PEA.
    // llvm.reachability_fence virtualize. Status: NOT WIRED — the upstream
    // LLVM tree this Jeandle fork tracks does NOT define
    // Intrinsic::reachability_fence (verified against
    // build-debug/include/llvm/IR/IntrinsicEnums.inc) and the Jeandle
    // frontend does not emit any analogue call (no caller of
    // Reference.reachabilityFence currently lowers to a tracked intrinsic;
    // jeandleAbstractInterpreter has no reachability-fence path). When LLVM
    // adds the intrinsic (or Jeandle introduces `jeandle.reachability_fence`),
    // add the matcher here BEFORE the JavaOp-fold and generic-escape paths.
    //
    // Expected behaviour, paraphrased from a reference virtualize:
    //   For each arg V of the fence:
    //     - V is a constant or primitive          -> drop arg
    //     - V is a non-virtual pointer            -> keep arg unchanged
    //     - V resolves to a virtual ObjectID OID  -> visit OID once and
    //         recursively process every entry of its VirtualObject's
    //         ObjectState. Entries that are FieldValue::scalar(Value*) or
    //         FieldValue::materializedRef(Value*) are non-virtual leaves and
    //         become args of the rewritten fence; FieldValue::virtualRef(ID)
    //         entries recurse; FieldValue::unknown contributes nothing
    //         (default-zero field of a virtual, no live pointer to keep).
    //   If after rewriting the arg list is EMPTY -> delete the fence (every
    //   transitively-reachable leaf was primitive/constant; the fence has
    //   nothing to keep alive).
    //   If the arg list is non-empty but DIFFERENT from the original ->
    //   replace the original call with a new call to llvm.reachability_fence
    //   whose operands are the collected leaves.
    //   If unchanged -> no-op (treat like lifetime_end above).
    //
    // The rewrite needs a new PEAResult::EffectKind (e.g. RewriteCallArgs)
    // so the transform pass can swap the operand list at apply time; the
    // analyzer-side helper that walks the VO entry tree is intentionally NOT
    // pre-built here to avoid dead code in this fork (no production caller
    // exists). Tests are deferred until the intrinsic / Jeandle named call
    // lands.
    //
    // ----------------------------------------------------------------------
    // Wiring-point TODOs for the deferred virtualization handlers
    // (ObjectClone, GetClass/load-hub, EnsureVirtualized,
    // FinalFieldBarrier elide).
    // ----------------------------------------------------------------------
    // The following PEA folds map to frontend constructs that the current
    // Jeandle frontend does NOT emit. The intrinsic-name inventory
    // (grep `jeandle\\.[a-z_]+` in jeandle-jdk/src/hotspot/share/jeandle/)
    // contains: array_store_check, arraylength, card_table_barrier,
    // check_if_value_based, check_inflated, check_instanceof,
    // check_klass_subtype, check_klass_subtype_slow_path, checkcast,
    // clear_oop_in_lock_stack_top, current_thread, decrement_lock_count,
    // get_stack_pointer, idiv, increment_lock_count, instanceof, irem,
    // ldiv, load_klass, lrem, monitorenter_*, monitorexit_*, new_instance,
    // newarray, personality, post_barrier, pre_barrier, safepoint_poll,
    // try_acquire_monitor_lock, try_release_monitor_lock. No `clone`,
    // `get_class`, `final_field_barrier`, or `ensure_virtualized` JavaOp
    // exists, so the matchers below are intentionally absent from
    // tier2JavaOpFold to avoid dead code. Register-finalizer is modeled as
    // `jeandle.register_finalizer_if_needed` and folded below. When the
    // frontend grows the remaining JavaOps, wire each fold here and add the
    // isJeandle* predicate in PartialEscapeUtils.{h,cpp}.
    //
    // ObjectClone — clone virtualizer that duplicates the receiver's VO
    // ObjectState when virtual, else stages per-field loads from the
    // materialized receiver.
    //   Wiring sketch when `jeandle.clone(oop)` lands:
    //     auto BaseID = jeandle::pea::resolveVirtualRef(CB->getArgOperand(0),
    //                                                   CurrentState,
    //                                                   Aliases, DL);
    //     if (!BaseID) return false;          // receiver already escaped
    //     auto &Src = *Result.VirtualObjects[*BaseID];
    //     auto NewVO = Src.duplicate();       // already implemented; copies
    //                                         // Klass, Fields, Array{Length,
    //                                         // ElementType, IndexScale,
    //                                         // BaseOffset}, IsSynthetic=false.
    //     jeandle::ObjectID NID = registerVirtualObject(std::move(NewVO),
    //                                                   /*alloc=*/CB);
    //     CurrentState.addObject(NID,
    //         CurrentState.getObjectState(*BaseID));    // copies FieldStates +
    //                                                   // LockCount=0 reset.
    //     Aliases.addVirtualAlias(CB, NID);
    //     emitEliminateAllocation(CB, NID);   // clone call becomes the new
    //                                         // alloc seed (same effect kind
    //                                         // as tier1Allocate).
    //   Tests: 320-329 reserved.
    //
    // Object.getClass / load-hub — get-class virtualize returns the
    // java.lang.Class mirror constant when the receiver is a virtual with
    // known Klass. Existing foldLoadKlass already covers the raw Klass*
    // pointer case (see jeandle.load_klass dispatch above).
    //   Wiring sketch when `jeandle.get_class(oop)` lands:
    //     auto BaseID = jeandle::pea::resolveVirtualRef(CB->getArgOperand(0),
    //                                                   CurrentState,
    //                                                   Aliases, DL);
    //     if (!BaseID) return false;
    //     auto &VObj = *Result.VirtualObjects[*BaseID];
    //     if (VObj.Klass == 0) return false;
    //     // New VMCallback required:
    //     //   uintptr_t (*JavaClassMirror)(uintptr_t klass);  // klass ->
    //     //                                                  // Class oop addr
    //     const auto *VC = jeandle::getVMCallbacks();
    //     if (!VC || !VC->JavaClassMirror) return false;
    //     uintptr_t Mirror = VC->JavaClassMirror(VObj.Klass);
    //     if (Mirror == 0) return false;
    //     LLVMContext &Ctx = F.getContext();
    //     Type *PtrTy = PointerType::get(Ctx,
    //                                     jeandle::AddrSpace::JavaHeapAddrSpace);
    //     Constant *MirrorPtr = ConstantExpr::getIntToPtr(
    //         ConstantInt::get(Type::getInt64Ty(Ctx), Mirror), PtrTy);
    //     emitReplaceCall(CB, MirrorPtr, *BaseID);
    //     return true;
    //   Tests: 320-329 reserved.
    //
    // FinalFieldBarrier elide -- virtualize deletes a final-field publication
    // barrier when the receiver is virtual. No frontend producer exists yet
    // (`jeandle.final_field_barrier` is not emitted), so this remains
    // deferred. Register-finalizer is handled by
    // foldRegisterFinalizerIfNeeded once the frontend emits the explicit
    // `jeandle.register_finalizer_if_needed(oop)` JavaOp.
    //   Wiring sketch when `jeandle.final_field_barrier` lands:
    //     if (isJeandleFinalFieldBarrier(CB)) {
    //       auto BaseID = jeandle::pea::resolveVirtualRef(CB->getArgOperand(0),
    //                                                     CurrentState,
    //                                                     Aliases, DL);
    //       if (!BaseID) return false;
    //       emitReplaceCall(CB, nullptr, *BaseID);
    //       return true;
    //     }
    //   Tests: 320-329 reserved.
    //
    // EnsureVirtualized — lets clients demand PEA succeed for a specific
    // oop; on failure the compilation bails. The loop fixpoint's
    // MATERIALIZE_ALL fallback is the "would-have-to-materialize" point
    // and is now in place, so Phase 2 is technically actionable.
    //   Status: DEFERRED. No frontend producer exists. There is no
    //   `jeandle.ensure_virtualized` JavaOp and no other path that marks a
    //   VirtualObject as must-stay-virtual. Adding the
    //   `EnsureVirtualized` bit on VirtualObject + the
    //   MATERIALIZE_ALL-time hard-bail check would be dead code in this
    //   fork (the flag is never set, so the check never fires). This
    //   matches the project rule "no dead code without a production
    //   caller" already applied to the reachability_fence wiring above.
    //   Wiring sketch when frontend producer lands:
    //     Phase 1: add `bool EnsureVirtualized = false;` to VirtualObject
    //              (PartialEscape.h §1.1, alongside MustPreserveLocks /
    //              IdentityHashObserved); duplicate() copies it.
    //     Phase 2: in processLoop's MATERIALIZE_ALL fallback, BEFORE
    //              swapping CurrentMode to Mode::MaterializeAll, walk
    //              every still-Eligible VO in CurrentState and if any has
    //              EnsureVirtualized=true, abort PEA for this function
    //              (clear Result.BlockEffects + Result.VirtualObjects,
    //              skip commit, emit a debug diagnostic). The cleanest
    //              integration point is a new PEAResult flag
    //              (e.g. `bool Bailed = false;`) consulted at the top of
    //              PartialEscapeTransform::apply.
    //     Phase 3: frontend adds `jeandle.ensure_virtualized(oop)`. Match
    //              it in tier2JavaOpFold: resolve receiver to a VO, set
    //              EnsureVirtualized on that VO, replace the call with
    //              undef (the node is purely advisory).
    //
    // §2.3.11/§2.3.12: equality compare against a virtual pointer folds.
    // Virtual objects are never null (by construction they track an in-flight
    // alloc), so `icmp eq virt, null` -> false, `icmp ne virt, null` -> true.
    // Two virtuals: same ID -> eq=true; different IDs -> eq=false.
    // Mixed virtual + non-null non-virtual pointer: identity differs -> eq
    // folds to false.
    if (auto *ICmp = dyn_cast<ICmpInst>(I)) {
      if (ICmp->isEquality()) {
        Value *Op0 = ICmp->getOperand(0);
        Value *Op1 = ICmp->getOperand(1);
        auto V0 = jeandle::pea::resolveVirtualRef(Op0, CurrentState, Aliases,
                                                  DL);
        auto V1 = jeandle::pea::resolveVirtualRef(Op1, CurrentState, Aliases,
                                                  DL);
        bool Op0IsNull = isa<ConstantPointerNull>(Op0);
        bool Op1IsNull = isa<ConstantPointerNull>(Op1);
        bool Folded = false;
        bool EqResult = false;
        jeandle::ObjectID BaseID = jeandle::InvalidObjectID;
        // Structural-equals fold for boxed primitive virtuals.
        // When both operands resolve to DIFFERENT virtual IDs whose Klass
        // is one of the eight java.lang autobox wrapper classes AND both
        // wrap the same primitive kind AND neither carries any live
        // monitor lock, replace the identity comparison with a value
        // comparison over the boxed primitives (when both inputs are
        // boxed virtuals of the same JavaKind, the identity check is
        // semantically equivalent to comparing the unboxed values).
        //
        // PEA always creates a fresh virtual at each `jeandle.new_instance`
        // (no box-cache lookup is modeled in the analyzer), so the
        // identity-based path below would conservatively fold to false
        // for two distinct boxed VOs even when they wrap equal primitive
        // values — strictly weaker than what Java's
        // `Integer.valueOf(x).equals(Integer.valueOf(y))` semantics
        // imply. The structural fold restores precision; soundness
        // follows from "two virtuals never escape into the box cache
        // before fold time" (PEA materializes each VO into a fresh oop
        // until Phase 5 lands, but the fold here runs BEFORE any
        // materialization decision and the icmp's result is the only
        // visible effect we replace).
        //
        // Lit tests that don't register an IsBoxed VMCallback leave
        // BoxedPrimitiveKind at the JBasicType::Count sentinel and this
        // path stays inert (the identity-based folds below still fire as
        // before).
        bool BoxedFolded = false;
        if (V0 && V1 && *V0 != *V1) {
          jeandle::VirtualObject &VO0 = *Result.VirtualObjects[*V0];
          jeandle::VirtualObject &VO1 = *Result.VirtualObjects[*V1];
          if (VO0.isBoxedPrimitive() &&
              VO0.BoxedPrimitiveKind == VO1.BoxedPrimitiveKind &&
              Eligible.lookup(*V0) && Eligible.lookup(*V1) &&
              LockCounts.lookup(*V0) == 0 && LockCounts.lookup(*V1) == 0) {
            jeandle::JBasicType Kind =
                static_cast<jeandle::JBasicType>(VO0.BoxedPrimitiveKind);
            Type *ValTy = jeandle::pea::llvmElementTypeFor(Kind,
                                                            F.getContext());
            if (ValTy) {
              // Look up the boxed value from FieldStates: a boxed wrapper
              // class has exactly one primitive instance field
              // (Integer.value / Long.value / ...). We accept either the
              // recorded scalar (when the constructor + store have been
              // virtualized) or the default zero of the primitive type
              // (when the value field was never explicitly stored — Java
              // default semantics for an uninitialised primitive field).
              auto getBoxedValue = [&](jeandle::ObjectID ID) -> Value * {
                auto FIt = FieldStates.find(ID);
                if (FIt != FieldStates.end()) {
                  // Pick a scalar entry whose type matches the boxed
                  // primitive. We do not assume a specific offset (HotSpot
                  // sets java_lang_boxing_object::value_offset() at
                  // VM-init time; without a VMCallback giving us that
                  // value we instead key on type identity). Iterate the
                  // map's recorded pairs deterministically by lowest
                  // offset to avoid any DenseMap ordering nondeterminism
                  // (we are about to issue a fold whose IR shape must be
                  // stable across runs / replays).
                  SmallVector<int64_t, 4> Offs;
                  Offs.reserve(FIt->second.size());
                  for (auto &Kv : FIt->second)
                    Offs.push_back(Kv.first);
                  llvm::sort(Offs);
                  for (int64_t Off : Offs) {
                    const jeandle::FieldValue &FV = FIt->second.lookup(Off);
                    if (FV.isScalar() && FV.getScalar()->getType() == ValTy)
                      return FV.getScalar();
                  }
                }
                // No matching scalar found — default-zero per Java
                // primitive-field initialisation.
                return jeandle::FieldValue::defaultFor(ValTy);
              };
              Value *VA = getBoxedValue(*V0);
              Value *VB = getBoxedValue(*V1);
              if (VA && VB && VA->getType() == ValTy &&
                  VB->getType() == ValTy) {
                bool IsEqPred =
                    (ICmp->getPredicate() == ICmpInst::ICMP_EQ);
                Value *Repl = nullptr;
                if (auto *CA = dyn_cast<Constant>(VA)) {
                  if (auto *CB = dyn_cast<Constant>(VB)) {
                    // Constant-fold the structural comparison.
                    Repl = ConstantFoldCompareInstOperands(
                        IsEqPred ? ICmpInst::ICMP_EQ : ICmpInst::ICMP_NE,
                        CA, CB, DL);
                  }
                }
                if (!Repl) {
                  // Runtime structural compare: build an unparented ICmpInst
                  // whose operands are the boxed primitive values, and let
                  // the transform's ReplaceLoad handler splice it in just
                  // before the original pointer ICmp. Mirrors the
                  // coerceToType ownership pattern (see OwnedInsts in
                  // PartialEscape.h).
                  Repl = CmpInst::Create(
                      Instruction::ICmp,
                      IsEqPred ? ICmpInst::ICMP_EQ : ICmpInst::ICMP_NE,
                      VA, VB, "pea.box.eq", /*InsertBefore=*/(Instruction *)nullptr);
                  Repl->setName("pea.box.eq");
                  if (ICmp->getDebugLoc())
                    cast<Instruction>(Repl)->setDebugLoc(ICmp->getDebugLoc());
                  Result.OwnedInsts.emplace_back(cast<Instruction>(Repl));
                }
                if (Repl) {
                  jeandle::PEAResult::Effect E;
                  E.Kind = jeandle::PEAResult::EffectKind::ReplaceLoad;
                  E.Block = ICmp->getParent();
                  E.Target = ICmp;
                  E.Replacement = Repl;
                  E.SeqNo = Result.nextSeqNo();
                  E.ObjID = *V0;
                  Result.addBlockEffect(std::move(E));
                  if (auto *RC = dyn_cast<Constant>(Repl))
                    Aliases.addScalarAlias(ICmp, RC);
                  BoxedFolded = true;
                }
              }
            }
          }
        }
        if (BoxedFolded)
          return;
        if (V0 && Op1IsNull) {
          Folded = true;
          EqResult = false;
          BaseID = *V0;
        } else if (V1 && Op0IsNull) {
          Folded = true;
          EqResult = false;
          BaseID = *V1;
        } else if (V0 && V1) {
          Folded = true;
          EqResult = (*V0 == *V1);
          BaseID = *V0;
        } else if (V0 && !V1 && !Op1IsNull) {
          // Virtual vs. non-virtual non-null pointer: distinct identity.
          Folded = true;
          EqResult = false;
          BaseID = *V0;
        } else if (V1 && !V0 && !Op0IsNull) {
          Folded = true;
          EqResult = false;
          BaseID = *V1;
        }
        if (Folded) {
          bool IsEq = (ICmp->getPredicate() == ICmpInst::ICMP_EQ);
          bool FinalResult = IsEq ? EqResult : !EqResult;
          Constant *C = ConstantInt::get(ICmp->getType(),
                                         FinalResult ? 1 : 0);
          // Reuse ReplaceLoad: its handler does generic Instruction RAUW +
          // erase, which is exactly what we need here.
          jeandle::PEAResult::Effect E;
          E.Kind = jeandle::PEAResult::EffectKind::ReplaceLoad;
          E.Block = ICmp->getParent();
          E.Target = ICmp;
          E.Replacement = C;
          E.SeqNo = Result.nextSeqNo();
          E.ObjID = BaseID;
          Result.addBlockEffect(std::move(E));
          Aliases.addScalarAlias(ICmp, C);
          return;
        }
      }
      // Non-equality ICmp on virtual heap pointers (slt/sgt/...) is UB on
      // GC pointers; conservatively materialize.
    }
    // Recognise JavaOps that read/inspect a virtual receiver and try to
    // constant-fold them. tier2JavaOpFold returns true if the JavaOp was
    // handled (whether by folding to a constant or by being a known-safe
    // non-escaping shape that needs no transform).
    if (auto *CB = dyn_cast<CallBase>(I)) {
      if (tier2JavaOpFold(CB))
        return;
      // Fall through to the generic-escape path for unrecognised calls.
    }
    // Any other consumer of a virtual operand triggers materialization.
    materializeAllVirtualOperands(I);
    return;
  }

  // Tier 3 (scalar-replaced inputs): nothing to do.
}

void Analyzer::tier1Allocate(CallBase *CB) {
  // In StopNewInLoopNest mode (transiently set by processLoop at a
  // top-level nest whose maximum depth exceeds JeandlePEALoopCutoff),
  // refuse to register NEW virtual allocations inside the nest, but leave
  // every other state intact — already-virtual objects (registered in a
  // shallower enclosing scope, or before the nest entry) continue to be
  // tracked and folded as usual.
  // The EnsureVirtualized override is deferred.
  if (CurrentMode == Mode::StopNewInLoopNest)
    return;

  // In MATERIALIZE_ALL mode the analyzer registers the VO normally
  // (so intra-block tier2Load/tier2Store folds against the new FieldStates),
  // then defers a Materialize effect to end-of-processBlock so the alloc is
  // re-emitted at the block's terminator IP — by which time all stores have
  // updated FieldStates so the materialised invoke captures the final field
  // values. In MATERIALIZE_ALL, a virtualizable node is virtualised AND
  // immediately ensure-materialized before the next fixed node. The
  // end-of-block emission is the practical compromise: intra-block folds
  // work, end-of-block emission is dominance-safe.
  const bool VirtualiseThenMaterialise =
      (CurrentMode == Mode::MaterializeAll);

  // Stable VO-per-allocation-site cache. Re-processing the same alloc
  // inside a loop fixpoint iteration must produce the same ObjectID so the
  // per-block exit snapshots can be compared for convergence. The cache is
  // monotonic (entries are never removed), and ineligibility carries over
  // across rollback boundaries.
  if (auto It = AllocSiteToVO.find(CB); It != AllocSiteToVO.end()) {
    jeandle::ObjectID ID = It->second;
    if (!Eligible.lookup(ID))
      return; // poisoned in a prior iteration — leave the original alloc.
    // Re-register the alias and the virtual ObjectState in CurrentState.
    // Aliases.addVirtualAlias asserts !already-aliased, so call only when
    // the cache restore has wiped the entry (the common case under
    // restoreLoopSnapshot, which restores Aliases to its pre-loop state).
    if (!Aliases.getVirtualAlias(CB))
      Aliases.addVirtualAlias(CB, ID);
    if (!CurrentState.hasObjectState(ID))
      CurrentState.addObject(ID, jeandle::ObjectState(/*numEntries=*/0));
    // Re-emit the EliminateAllocation effect. The pre-iter snapshot has
    // wiped BlockEffects[CB->getParent()] of this iteration's prior copy,
    // and addBlockEffect doesn't dedup, so this is exactly the right place.
    jeandle::PEAResult::Effect E;
    E.Kind = jeandle::PEAResult::EffectKind::EliminateAllocation;
    E.Block = CB->getParent();
    E.Target = CB;
    E.SeqNo = Result.nextSeqNo();
    E.ObjID = ID;
    Result.addBlockEffect(std::move(E));
    // Also re-enqueue the per-block materialise on the cache-hit
    // path. For an InvokeInst alloc, the alloc IS the terminator of its
    // block, so we cannot drain at end-of-current-block (the alloc only
    // gets registered AFTER our drain hook would fire). Defer to the
    // NORMAL-dest block: that's where downstream tier2Load/tier2Store
    // fold the VO's FieldStates, and end-of-that-block is the correct
    // dominance-safe materialise IP.
    if (VirtualiseThenMaterialise) {
      BasicBlock *MatBB = CB->getParent();
      if (auto *II = dyn_cast<InvokeInst>(CB))
        MatBB = II->getNormalDest();
      PendingMaterializeAllVOs[MatBB].push_back(ID);
    }
    return;
  }

  // Loop-body allocations are permitted as virtualization candidates. The
  // dominance check in materializeAt (DT.dominates(VI, SafeIP) per
  // FieldStates entry) is what protects the analyzer from forming an unsound
  // replay: any field whose stored value is defined later in the loop body
  // cannot dominate the SafeIP just after the alloc, so the object becomes
  // ineligible and survives in IR untouched. materializeBeforeLoops
  // independently force-materializes any virtual that is still virtual at a
  // loop preheader's exit (modulo loops drained by the loop fixpoint's
  // pessimistic fallback), so objects allocated BEFORE the loop and
  // surviving into the loop are also handled.

  uintptr_t Klass = jeandle::pea::extractAllocationKlass(CB);
  if (Klass == 0)
    return;

  const bool IsInstance = jeandle::pea::isJeandleNewInstance(CB);
  const bool IsArray = jeandle::pea::isJeandleNewArray(CB);
  assert((IsInstance ^ IsArray) && "allocation must be either instance or array");

  // Refuse to virtualize identity-sensitive allocations.
  // HasFinalizer: classes that override finalize() require HotSpot's
  // RegisterFinalizer hook to fire at the original allocation site;
  // eliding the alloc would skip finalizer registration and break
  // user-visible semantics.
  // CanVirtualize: Reference / Thread (and subtypes) have lifecycle
  // tracked by global runtime state (pending-reference list, thread
  // list); virtualizing them would silently elide that tracking.
  // Both checks fire only on instance allocations (arrays cannot have
  // finalizers, and HotSpot's finalizer / Reference / Thread machinery
  // is keyed on InstanceKlass identity). Lit tests without a cblog
  // continue to virtualize as before because both pointers are
  // nullptr and we fall through to the legacy default-virtualize path.
  if (IsInstance) {
    if (const jeandle::VMCallbacks *VMCB = jeandle::getVMCallbacks()) {
      if (VMCB->HasFinalizer && VMCB->HasFinalizer(Klass))
        return;
      if (VMCB->CanVirtualize && !VMCB->CanVirtualize(Klass))
        return;
    }
  }

  std::unique_ptr<jeandle::VirtualObject> VO;

  if (IsInstance) {
    auto Size = jeandle::pea::extractInstanceSize(CB);
    if (!Size)
      return;
    VO = std::make_unique<jeandle::VirtualObject>(
        jeandle::InvalidObjectID, jeandle::VirtualObject::Instance, CB);
    VO->Klass = Klass;
    VO->SizeInBytes = *Size;
    // Tag this Instance VO with the boxed-primitive kind if
    // its Klass is one of the eight java.lang autobox wrapper classes.
    // The IsBoxed VMCallback returns the JBasicType integer of the boxed
    // primitive (0..7) for boxed classes and 9 (JBasicType::Count) for
    // everything else; we store the value verbatim so isBoxedPrimitive()
    // is a single equality check. When the VMCallback isn't registered
    // (lit tests without a cblog or callback wiring), the kind stays at
    // the default 9 (Count) and the boxed-virtual fold paths stay inert.
    if (const jeandle::VMCallbacks *VMCB = jeandle::getVMCallbacks()) {
      if (VMCB->IsBoxed) {
        int Kind = VMCB->IsBoxed(Klass);
        if (Kind >= 0 &&
            Kind < static_cast<int>(jeandle::JBasicType::Count))
          VO->BoxedPrimitiveKind = static_cast<uint8_t>(Kind);
      }
    }
  } else {
    auto Length = jeandle::pea::extractArrayLength(CB);
    if (!Length)
      return;
    if (*Length > MaximumEscapeAnalysisArrayLength)
      return;
    VO = std::make_unique<jeandle::VirtualObject>(
        jeandle::InvalidObjectID, jeandle::VirtualObject::Array, CB);
    VO->Klass = Klass;
    VO->ArrayLength = *Length;
    // VO->ArrayLengthVal was a placeholder for symbolic-length
    // arrays; never read by the analyzer or transform.
    // Populate per-element metadata so matchArrayElementGEP can match
    // typed-GEP / symbolic-byte-offset element accesses. If the VMCallback
    // is unregistered or cannot identify the element kind, leave
    // ArrayElementType nullptr — matchArrayElementGEP will refuse to fire
    // and only constant-byte-offset element accesses (handled directly by
    // resolveFieldOffset) will be eligible.
    if (auto Kind = jeandle::pea::elementTypeForArrayKlass(Klass)) {
      // VMConstants are now read out of the module's runtime-defined
      // globals (patched by HotSpot's
      // RuntimeDefinedJavaOps::define_global_variables); see
      // llvm/IR/Jeandle/VMConstants.h for the delivery model. Lit tests
      // that never link the template module fall through to the
      // compile-time defaults declared on `struct VMConstants`.
      const jeandle::VMConstants VMConsts =
          jeandle::VMConstants::fromModule(*F.getParent());
      Type *ElemTy =
          jeandle::pea::llvmElementTypeFor(*Kind, F.getContext());
      if (ElemTy) {
        VO->ArrayElementType = ElemTy;
        VO->ArrayBaseOffset = static_cast<uint32_t>(
            VMConsts.arrayBaseOffsetFor(*Kind));
        VO->ArrayIndexScale = static_cast<uint32_t>(
            VMConsts.elementSizeFor(*Kind));
      }
    }
  }

  jeandle::ObjectID ID = Result.createVirtualObject(std::move(VO));
  AllocSiteToVO[CB] = ID;
  Aliases.addVirtualAlias(CB, ID);
  // Register a Virtual ObjectState with zero entries. resolveVirtualRef only
  // needs the slot to be present + Kind == Virtual; the per-field FieldValue
  // tracking lives in FieldStates (see class comment).
  CurrentState.addObject(ID, jeandle::ObjectState(/*numEntries=*/0));
  Eligible[ID] = true;

  jeandle::PEAResult::Effect E;
  E.Kind = jeandle::PEAResult::EffectKind::EliminateAllocation;
  E.Block = CB->getParent();
  E.Target = CB;
  E.SeqNo = Result.nextSeqNo();
  E.ObjID = ID;
  Result.addBlockEffect(std::move(E));

  ++Result.VirtualizationDelta;
  --Result.AllocationDelta;
  // A new virtual object was registered.
  ++JeandlePEAVirtualized;

  // Enqueue end-of-block materialise under VirtualiseThenMaterialise.
  // For an InvokeInst alloc the alloc IS the block terminator; drain in
  // the normal destination instead so subsequent field accesses in that
  // block fold first.
  if (VirtualiseThenMaterialise) {
    BasicBlock *MatBB = CB->getParent();
    if (auto *II = dyn_cast<InvokeInst>(CB))
      MatBB = II->getNormalDest();
    PendingMaterializeAllVOs[MatBB].push_back(ID);
  }
}

bool Analyzer::tier2Store(StoreInst *SI) {
  Value *Ptr = SI->getPointerOperand();
  Value *Val = SI->getValueOperand();

  auto BaseID = jeandle::pea::resolveVirtualRef(Ptr, CurrentState, Aliases, DL);
  if (!BaseID)
    return false;

  jeandle::VirtualObject &VObj = *Result.VirtualObjects[*BaseID];

  // For array VOs with populated element metadata, recognise the
  // typed-element GEP chain that the abstract interpreter emits for
  // indexed accesses. matchArrayElementGEP returns Some(idx, etype) on a
  // recognised array-element GEP shape; idx is a ConstantInt for constant
  // indices (use the canonical byte offset) and any other Value for
  // symbolic indices (force materialization, matching the
  // "index constant only" policy).
  std::optional<int64_t> Offset;
  if (VObj.isArray() && VObj.ArrayElementType) {
    if (auto *G = dyn_cast<GetElementPtrInst>(Ptr)) {
      if (auto Match = VObj.matchArrayElementGEP(G, DL)) {
        if (auto *CI = dyn_cast<ConstantInt>(Match->Index)) {
          int64_t Cidx = CI->getSExtValue();
          if (Cidx < 0 || static_cast<uint64_t>(Cidx) >= VObj.ArrayLength) {
            Eligible[*BaseID] = false;
            return true;
          }
          Offset = static_cast<int64_t>(VObj.ArrayBaseOffset) +
                   Cidx * static_cast<int64_t>(VObj.ArrayIndexScale);
        } else {
          // Symbolic index — materialize.
          Eligible[*BaseID] = false;
          return true;
        }
      }
    }
  }

  if (!Offset)
    Offset = jeandle::pea::resolveFieldOffset(Ptr, DL);
  if (!Offset) {
    // Non-constant offset access — punt on materialization.
    Eligible[*BaseID] = false;
    return true;
  }
  if (VObj.isInstance()) {
    const jeandle::VMConstants VMConsts =
        jeandle::VMConstants::fromModule(*F.getParent());
    if (*Offset < VMConsts.instanceBaseOffset()) {
      // Header accesses (mark/klass) are VM metadata, not Java fields.
      Eligible[*BaseID] = false;
      return true;
    }
  }

  // TODO: Unsafe.put{Int,Long,Short}-into-byte-array decomposition.
  // Re-enable together with the jeandle-jdk frontend inliner for
  // Unsafe.put* intrinsics.


  // Type-overlap validation via VirtualObject::getOrCreateFieldIndex. We don't
  // actually use the returned index (FieldStates is keyed by raw offset), but
  // -1 means an overlap/size conflict that forces escape.
  if (VObj.getOrCreateFieldIndex(*Offset, Val->getType()) < 0) {
    Eligible[*BaseID] = false;
    return true;
  }

  // Compute the FieldValue for the stored Value.
  if (auto RefID =
          jeandle::pea::resolveVirtualRef(Val, CurrentState, Aliases, DL)) {
    // Nested virtual reference. Recursive materialization handles this at
    // materialize time by first materializing the inner object then storing
    // its materialized pointer into the outer's field. We record the nested
    // reference here and let materializeAt rewrite it later.
    FieldStates[*BaseID][*Offset] =
        jeandle::FieldValue::virtualRef(*RefID, Val->getType());

    jeandle::PEAResult::Effect E;
    E.Kind = jeandle::PEAResult::EffectKind::EliminateStore;
    E.Block = SI->getParent();
    E.Target = SI;
    E.SeqNo = Result.nextSeqNo();
    E.ObjID = *BaseID;
    Result.addBlockEffect(std::move(E));
    return true;
  }
  FieldStates[*BaseID][*Offset] = jeandle::FieldValue::scalar(Val);

  jeandle::PEAResult::Effect E;
  E.Kind = jeandle::PEAResult::EffectKind::EliminateStore;
  E.Block = SI->getParent();
  E.Target = SI;
  E.SeqNo = Result.nextSeqNo();
  E.ObjID = *BaseID;
  Result.addBlockEffect(std::move(E));
  return true;
}

void Analyzer::tier2Load(LoadInst *LI) {
  Value *Ptr = LI->getPointerOperand();

  auto BaseID = jeandle::pea::resolveVirtualRef(Ptr, CurrentState, Aliases, DL);
  if (!BaseID)
    return;

  jeandle::VirtualObject &VObj = *Result.VirtualObjects[*BaseID];

  // Array-element GEP matcher — see the tier2Store mirror for the
  // rationale. Symbolic index forces the array to materialize.
  std::optional<int64_t> Offset;
  if (VObj.isArray() && VObj.ArrayElementType) {
    if (auto *G = dyn_cast<GetElementPtrInst>(Ptr)) {
      if (auto Match = VObj.matchArrayElementGEP(G, DL)) {
        if (auto *CI = dyn_cast<ConstantInt>(Match->Index)) {
          int64_t Cidx = CI->getSExtValue();
          if (Cidx < 0 || static_cast<uint64_t>(Cidx) >= VObj.ArrayLength) {
            Eligible[*BaseID] = false;
            return;
          }
          Offset = static_cast<int64_t>(VObj.ArrayBaseOffset) +
                   Cidx * static_cast<int64_t>(VObj.ArrayIndexScale);
        } else {
          Eligible[*BaseID] = false;
          return;
        }
      }
    }
  }

  if (!Offset)
    Offset = jeandle::pea::resolveFieldOffset(Ptr, DL);
  if (!Offset) {
    Eligible[*BaseID] = false;
    return;
  }
  if (VObj.isInstance()) {
    const jeandle::VMConstants VMConsts =
        jeandle::VMConstants::fromModule(*F.getParent());
    if (*Offset < VMConsts.instanceBaseOffset()) {
      // Header accesses (mark/klass) are VM metadata, not Java fields.
      Eligible[*BaseID] = false;
      return;
    }
  }

  Type *LoadTy = LI->getType();

  // Locate the FieldDesc whose recorded range contains the load. The
  // load may be at a sub-offset / sub-width of a wider stored entry (e.g.
  // store i64 at off 8, load i16 at off 12 → entry at off 8 with within-slot
  // byte offset 4). If the load straddles slot boundaries (overlaps without
  // being contained), we bail — that would need multi-slot read+concat.
  uint64_t LoadBits = LoadTy->isSized() ? DL.getTypeSizeInBits(LoadTy) : 0;
  uint8_t LoadByteSize = static_cast<uint8_t>((LoadBits + 7) / 8);
  int64_t LoadEnd = *Offset + static_cast<int64_t>(LoadByteSize);
  int64_t EntryOffset = *Offset;
  bool OverlapsNoncontained = false;
  // Fields are sorted by Offset; linear scan is fine for the small per-object
  // field counts we see in practice.
  for (const auto &F : VObj.Fields) {
    int64_t FEnd = F.Offset + static_cast<int64_t>(F.ByteSize);
    if (FEnd <= *Offset)
      continue;
    if (F.Offset >= LoadEnd)
      break;
    // Some overlap. Contained iff F covers [*Offset, LoadEnd).
    if (F.Offset <= *Offset && FEnd >= LoadEnd) {
      EntryOffset = F.Offset;
    } else {
      OverlapsNoncontained = true;
    }
    break;
  }
  if (OverlapsNoncontained) {
    // TODO: Unsafe.get{Int,Long,Short}-from-byte-array reassembly.
    // Re-enable together with the jeandle-jdk frontend inliner for
    // Unsafe.get* intrinsics. Until then any straddling load
    // conservatively forces materialization.
    Eligible[*BaseID] = false;
    return;
  }

  int64_t WithinSlotByteOff = *Offset - EntryOffset;

  const jeandle::FieldValue *Existing = nullptr;
  auto It = FieldStates.find(*BaseID);
  if (It != FieldStates.end()) {
    auto It2 = It->second.find(EntryOffset);
    if (It2 != It->second.end())
      Existing = &It2->second;
  }

  if (!Existing || Existing->isUnknown()) {
    // Default value for a never-written field.
    Constant *Def = jeandle::FieldValue::defaultFor(LoadTy);
    jeandle::PEAResult::Effect E;
    E.Kind = jeandle::PEAResult::EffectKind::ReplaceLoad;
    E.Block = LI->getParent();
    E.Target = LI;
    E.Replacement = Def;
    E.SeqNo = Result.nextSeqNo();
    E.ObjID = *BaseID;
    Result.addBlockEffect(std::move(E));
    Aliases.addScalarAlias(LI, Def);
    return;
  }

  if (Existing->isScalar()) {
    Value *V = Existing->getScalar();
    // Coerce to LoadTy. Handles same-bit-width primitive↔primitive (bitcast)
    // and sub-bit-width integer truncation (lshr+trunc at within-slot
    // byte offset). Pointer↔primitive (or cross-AS pointer↔pointer, or a
    // widening integer load) bails to ineligible per the stable-slot-kind
    // and sub-bit-width policies.
    Value *Coerced = coerceToType(V, LoadTy, LI, WithinSlotByteOff);
    if (!Coerced) {
      Eligible[*BaseID] = false;
      return;
    }
    jeandle::PEAResult::Effect E;
    E.Kind = jeandle::PEAResult::EffectKind::ReplaceLoad;
    E.Block = LI->getParent();
    E.Target = LI;
    E.Replacement = Coerced;
    E.SeqNo = Result.nextSeqNo();
    E.ObjID = *BaseID;
    Result.addBlockEffect(std::move(E));
    Aliases.addScalarAlias(LI, Coerced);
    return;
  }

  if (Existing->isVirtualRef()) {
    // Nested-virtual load: loading a field whose tracked value is another
    // virtual yields that other virtual (still virtual!) and forwards the
    // load to it. Forward the load to the inner virtual's allocation
    // Value and install a virtual alias from the load to
    // InnerID so downstream tier2 handlers (foldArrayLength, foldLoadKlass,
    // etc.) and tier3 escape detection see %loaded as a reference to the
    // inner virtual. If the inner later materializes / escapes, the
    // analyzer's existing nested-virtual machinery (a) rewrites every
    // other tracking site (FieldStates, alias map) to the materialized
    // pointer, and (b) at transform time, applyMaterialize's RAUW on
    // OrigInnerAlloc → NewInv redirects the just-RAUW'd uses to the new
    // materialized invoke. (Belt-and-suspenders: the ReplaceLoad handler
    // also looks up E.Replacement through NewAllocFor.)
    jeandle::ObjectID InnerID = Existing->getVirtualRef();

    if (!Eligible.lookup(InnerID)) {
      // The inner was abandoned by some upstream decision — its alloc
      // will survive in IR, but we should not silently keep forwarding to
      // it for the outer because forwarding can mask a missing
      // materialization. Bail conservatively on the outer.
      Eligible[*BaseID] = false;
      return;
    }

    const jeandle::ObjectState *InnerOS =
        CurrentState.getObjectStateOptional(InnerID);
    if (!InnerOS) {
      // The inner's ObjectState isn't live in this block (shouldn't happen
      // for a VirtualRef field entry that was inherited alongside the
      // outer, but defend). Bail on the outer only; do not poison the
      // inner — it may be cleanly virtualizable on other paths.
      Eligible[*BaseID] = false;
      return;
    }

    Value *Repl = nullptr;
    if (InnerOS->isMaterialized()) {
      // Defensive: the analyzer rewrites VirtualRef → MaterializedRef on
      // every materialization site, so a VirtualRef field entry whose
      // inner has flipped to Materialized at this point would normally be
      // stale. If we still observe it, fall back to the materialized
      // pointer — same shape as the MaterializedRef branch below.
      Repl = InnerOS->getMaterializedValue();
    } else {
      jeandle::VirtualObject &InnerVO = *Result.VirtualObjects[InnerID];
      Repl = InnerVO.AllocationCall;
    }
    if (!Repl) {
      Eligible[*BaseID] = false;
      return;
    }

    // Type-compatibility. For ordinary reference loads, both LoadTy and the
    // inner allocation are `ptr addrspace(1)` and coerceToType returns Repl
    // unchanged. Cross-address-space or ptr↔primitive mismatch bails per
    // the stable-slot-kind invariant; a nonzero WithinSlotByteOff also
    // bails (partial pointer loads are not virtualizable). We don't poison
    // InnerID because other paths may still be able to virtualize it.
    Value *Coerced = coerceToType(Repl, LoadTy, LI, WithinSlotByteOff);
    if (!Coerced) {
      Eligible[*BaseID] = false;
      return;
    }

    jeandle::PEAResult::Effect E;
    E.Kind = jeandle::PEAResult::EffectKind::ReplaceLoad;
    E.Block = LI->getParent();
    E.Target = LI;
    E.Replacement = Coerced;
    E.SeqNo = Result.nextSeqNo();
    E.ObjID = *BaseID;
    Result.addBlockEffect(std::move(E));

    // Install the virtual alias only when the inner is still virtual at
    // this point. If we fell back to a materialized pointer above, mirror
    // the MaterializedRef branch and register a scalar alias so downstream
    // resolveVirtualRef queries see through to the materialized value.
    if (InnerOS->isMaterialized())
      Aliases.addScalarAlias(LI, Coerced);
    else
      Aliases.addVirtualAlias(LI, InnerID);
    return;
  }

  if (Existing->isMaterializedRef()) {
    // A virtual's reference field carries a materialized pointer (e.g.
    // produced by a field-PHI synthesis at a merge block). Forward the load
    // to the materialized value, matching the Scalar handler.
    Value *V = Existing->getMaterialized();
    if (!V) {
      Eligible[*BaseID] = false;
      return;
    }
    // A materialized ref slot can only be loaded back as a pointer (and in
    // practice, since LLVM 17 uses opaque pointers, only as the same
    // ptr-AS). coerceToType bails on ptr↔primitive (stable-slot-kind),
    // cross-AS pointer pairs, and on any partial (within-slot) pointer
    // load.
    Value *Coerced = coerceToType(V, LoadTy, LI, WithinSlotByteOff);
    if (!Coerced) {
      Eligible[*BaseID] = false;
      return;
    }
    jeandle::PEAResult::Effect E;
    E.Kind = jeandle::PEAResult::EffectKind::ReplaceLoad;
    E.Block = LI->getParent();
    E.Target = LI;
    E.Replacement = Coerced;
    E.SeqNo = Result.nextSeqNo();
    E.ObjID = *BaseID;
    Result.addBlockEffect(std::move(E));
    Aliases.addScalarAlias(LI, Coerced);
    return;
  }

  // Should be unreachable; FieldValue::Tag is a closed enum.
  Eligible[*BaseID] = false;
}

// ---------------------------------------------------------------------------
// Tier-2 JavaOp folding on virtual receivers.
// ---------------------------------------------------------------------------

void Analyzer::emitReplaceCall(CallBase *CB, Value *Replacement,
                               jeandle::ObjectID ID) {
  jeandle::PEAResult::Effect E;
  E.Kind = jeandle::PEAResult::EffectKind::ReplaceCall;
  E.Block = CB->getParent();
  E.Target = CB;
  E.Replacement = Replacement;
  E.SeqNo = Result.nextSeqNo();
  E.ObjID = ID;
  Result.addBlockEffect(std::move(E));
  // Scalar-alias non-void call values so downstream resolveVirtualRef queries
  // (e.g. another JavaOp later in the same block whose operand is the call
  // result) see through to the replacement constant. Void JavaOps can use a
  // null Replacement to request deletion only.
  if (Replacement)
    Aliases.addScalarAlias(CB, Replacement);
}

std::optional<bool> Analyzer::evalSubtypeRelation(uintptr_t SubKlass,
                                                  uintptr_t SuperKlass) {
  const jeandle::VMCallbacks *CB = jeandle::getVMCallbacks();
  if (!CB || !CB->IsSubtype || !CB->IsInterface)
    return std::nullopt;
  if (CB->IsSubtype(SubKlass, SuperKlass))
    return true;
  // Virtual objects always have an exact, concrete klass (we know the
  // allocation site). Pass Exact=true to areKlassesIncompatible.
  if (jeandle::areKlassesIncompatible(SubKlass, /*Exact=*/true, SuperKlass))
    return false;
  return std::nullopt;
}

bool Analyzer::foldArrayLength(CallBase *CB) {
  if (CB->arg_size() < 1)
    return false;
  auto BaseID = jeandle::pea::resolveVirtualRef(CB->getArgOperand(0),
                                                CurrentState, Aliases, DL);
  if (!BaseID)
    return false;
  jeandle::VirtualObject &VObj = *Result.VirtualObjects[*BaseID];
  if (!VObj.isArray())
    return false;
  Type *I32 = Type::getInt32Ty(F.getContext());
  Constant *Len = ConstantInt::get(I32, VObj.ArrayLength);
  emitReplaceCall(CB, Len, *BaseID);
  return true;
}

bool Analyzer::foldLoadKlass(CallBase *CB) {
  if (CB->arg_size() < 1)
    return false;
  auto BaseID = jeandle::pea::resolveVirtualRef(CB->getArgOperand(0),
                                                CurrentState, Aliases, DL);
  if (!BaseID)
    return false;
  jeandle::VirtualObject &VObj = *Result.VirtualObjects[*BaseID];
  if (VObj.Klass == 0)
    return false;
  LLVMContext &Ctx = F.getContext();
  Type *I64 = Type::getInt64Ty(Ctx);
  Type *PtrTy = PointerType::get(Ctx, jeandle::AddrSpace::CHeapAddrSpace);
  Constant *KlassAsInt = ConstantInt::get(I64, VObj.Klass);
  Constant *KlassPtr = ConstantExpr::getIntToPtr(KlassAsInt, PtrTy);
  emitReplaceCall(CB, KlassPtr, *BaseID);
  return true;
}

bool Analyzer::foldCheckCast(CallBase *CB) {
  if (CB->arg_size() < 2)
    return false;
  uintptr_t SuperK = jeandle::extractKlassConstant(CB->getArgOperand(0));
  if (SuperK == 0)
    return false;
  auto BaseID = jeandle::pea::resolveVirtualRef(CB->getArgOperand(1),
                                                CurrentState, Aliases, DL);
  if (!BaseID)
    return false;
  jeandle::VirtualObject &VObj = *Result.VirtualObjects[*BaseID];
  if (VObj.Klass == 0)
    return false;
  auto Folded = evalSubtypeRelation(VObj.Klass, SuperK);
  if (!Folded)
    return false;
  Constant *Res = *Folded ? ConstantInt::getTrue(CB->getType())
                          : ConstantInt::getFalse(CB->getType());
  emitReplaceCall(CB, Res, *BaseID);
  return true;
}

bool Analyzer::foldCheckInstanceOf(CallBase *CB) {
  // Same shape as foldCheckCast: (super_klass, oop) -> i1.
  return foldCheckCast(CB);
}

bool Analyzer::foldInstanceOf(CallBase *CB) {
  if (CB->arg_size() < 2)
    return false;
  uintptr_t SuperK = jeandle::extractKlassConstant(CB->getArgOperand(0));
  if (SuperK == 0)
    return false;
  auto BaseID = jeandle::pea::resolveVirtualRef(CB->getArgOperand(1),
                                                CurrentState, Aliases, DL);
  if (!BaseID)
    return false;
  jeandle::VirtualObject &VObj = *Result.VirtualObjects[*BaseID];
  if (VObj.Klass == 0)
    return false;
  // Virtual objects are non-null by construction, so we can collapse the
  // null-check inside instanceof and reduce to a pure subtype query.
  auto Folded = evalSubtypeRelation(VObj.Klass, SuperK);
  if (!Folded)
    return false;
  Type *I32 = Type::getInt32Ty(F.getContext());
  Constant *Res = ConstantInt::get(I32, *Folded ? 1 : 0);
  emitReplaceCall(CB, Res, *BaseID);
  return true;
}

// Read the `!jeandle.lock_depth` i32 metadata node from a
// monitorenter call site. The JDK frontend
// (JeandleAbstractInterpreter::shared_lock) attaches this on every emitted
// jeandle.monitorenter_with_* call; lit tests can attach it directly to
// exercise the depth-aware paths. Returns std::nullopt when the metadata is
// absent or malformed — caller falls back to the analyzer's Order proxy.
static std::optional<uint32_t> readBytecodeLockDepth(llvm::CallBase *CB) {
  llvm::MDNode *MD = CB->getMetadata("jeandle.lock_depth");
  if (!MD || MD->getNumOperands() == 0)
    return std::nullopt;
  if (auto *CAM = llvm::dyn_cast<llvm::ConstantAsMetadata>(MD->getOperand(0))) {
    if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(CAM->getValue())) {
      uint64_t V = CI->getZExtValue();
      // Sanity: depth fits in uint32 (realistic upper bound is the JVM
      // monitor-stack limit, far below 2^32). Larger values are clamped to
      // UINT32_MAX so the comparison still behaves monotonically.
      return static_cast<uint32_t>(std::min<uint64_t>(V, UINT32_MAX));
    }
  }
  return std::nullopt;
}

bool Analyzer::foldMonitorEnter(CallBase *CB) {
  if (CB->arg_size() < 1)
    return false;
  auto BaseID = jeandle::pea::resolveVirtualRef(CB->getArgOperand(0),
                                                CurrentState, Aliases, DL);
  if (!BaseID)
    return false;

  // Resolve the bytecode lock depth for this enter.
  //   * When the JDK frontend's !jeandle.lock_depth metadata is present, use
  //     the metadata value (the true Java-bytecode monitor depth, stable per
  //     call site and lexically consistent across paths).
  //   * Otherwise, fall back to FallbackBytecodeDepth: cache the FIRST
  //     NextLockEnterOrder value ever assigned to this call site and reuse
  //     it on every subsequent visit. Using NextLockEnterOrder directly is
  //     unsound across loop-fixpoint iterations (the counter advances on
  //     every re-push, which would mutate ObjectState::Locks[i].BytecodeDepth
  //     and break PEABlockState::equivalentTo convergence checks).
  std::optional<uint32_t> MaybeDepth = readBytecodeLockDepth(CB);
  uint32_t NewBytecodeDepth;
  if (MaybeDepth.has_value()) {
    NewBytecodeDepth = *MaybeDepth;
  } else {
    auto FIt = FallbackBytecodeDepth.find(CB);
    if (FIt != FallbackBytecodeDepth.end()) {
      NewBytecodeDepth = FIt->second;
    } else {
      NewBytecodeDepth = NextLockEnterOrder;
      FallbackBytecodeDepth[CB] = NewBytecodeDepth;
    }
  }

  // materializeVirtualLocksBefore pre-cascade. When we are
  // about to virtualise a monitorenter on a NEW receiver ID while another VO
  // already holds an OLDER (shallower-depth) elided lock, the runtime
  // lock-stack ordering observable across later escape points would be
  // silently reversed: the older VO would materialise alone at its escape
  // point without its sibling's lock on the stack. We force every sibling
  // holding a shallower-depth lock to materialise BEFORE the new virtual
  // lock is added.
  //
  // Compares BytecodeDepth rather than the legacy Order proxy.
  // When the metadata is absent on every call, BytecodeDepth == Order at
  // push time, so the behaviour is identical to the earlier implementation.
  if (StrictLockOrder) {
    SmallVector<jeandle::ObjectID, 4> ToPreCascade;
    for (auto &Kv : LiveLockEnters) {
      if (Kv.first == *BaseID)
        continue;
      if (Kv.second.empty())
        continue;
      if (Kv.second.front().BytecodeDepth < NewBytecodeDepth)
        ToPreCascade.push_back(Kv.first);
    }
    llvm::sort(ToPreCascade); // deterministic
    for (jeandle::ObjectID OID : ToPreCascade)
      materializeAt(OID, CB, MatReason::Cascade);
  }

  // Lock confinement: the lock counter is balanced per-block at commit
  // time. A monitorenter on a virtual is always safe to provisionally
  // elide; if the matching monitorexit is missing, commit() will flip the
  // virtual to ineligible and the effects will be dropped.
  ++LockCounts[*BaseID];
  // Push the elided enter onto the live stack so materializeAt can undo
  // only the unbalanced enters along this path if the object later escapes.
  // Tag every push with the next monotonic Order so the narrow cascade
  // rule (other.minOrder < this.maxOrder) can be evaluated against the live
  // stack at materialization time.
  // Also record the bytecode depth so the (depth-aware) cascade and
  // merge-time stack-identity comparisons use the JDK-supplied value.
  uint32_t MyOrder = NextLockEnterOrder++;
  LiveLockEnters[*BaseID].push_back({CB, MyOrder, NewBytecodeDepth});
  // Keep the per-VO ObjectState::Locks mirror in lockstep with the analyzer-
  // side DenseMap. ObjectState::Locks does not carry the Order proxy —
  // structural ObjectState equivalence (used by merge-time identicalObjectStates and
  // the PEABlockState::equivalentTo path) compares Call+BytecodeDepth only.
  if (CurrentState.hasObjectState(*BaseID)) {
    jeandle::ObjectState &OS =
        CurrentState.getObjectStateForModification(*BaseID);
    if (OS.isVirtual())
      OS.addLock({CB, NewBytecodeDepth});
  }
  Constant *True = ConstantInt::getTrue(CB->getType());
  emitReplaceCall(CB, True, *BaseID);
  return true;
}

bool Analyzer::foldMonitorExit(CallBase *CB) {
  if (CB->arg_size() < 1)
    return false;
  auto BaseID = jeandle::pea::resolveVirtualRef(CB->getArgOperand(0),
                                                CurrentState, Aliases, DL);
  if (!BaseID)
    return false;
  auto It = LockCounts.find(*BaseID);
  if (It == LockCounts.end() || It->second == 0) {
    // Unbalanced monitorexit (release without acquire on this virtual). Mark
    // the virtual ineligible and let the generic escape path keep the call.
    Eligible[*BaseID] = false;
    return false;
  }
  --It->second;
  // Pop the matching enter off the live stack. The pair is now balanced
  // and both calls' ReplaceCall(true) effects stay in place — the eventual
  // dead-code sweep erases them along with the original allocation.
  auto SIt = LiveLockEnters.find(*BaseID);
  assert(SIt != LiveLockEnters.end() && !SIt->second.empty() &&
         "live stack must be non-empty when LockCount > 0");
  SIt->second.pop_back();
  // Mirror the pop on ObjectState::Locks so the on-VO Locks view
  // stays consistent with the analyzer-side LiveLockEnters for any caller
  // that introspects the ObjectState directly (e.g. equivalentTo /
  // identicalObjectStates).
  if (CurrentState.hasObjectState(*BaseID)) {
    jeandle::ObjectState &OS =
        CurrentState.getObjectStateForModification(*BaseID);
    if (OS.isVirtual() && OS.hasLocks())
      OS.removeLock();
  }
  Constant *True = ConstantInt::getTrue(CB->getType());
  emitReplaceCall(CB, True, *BaseID);
  return true;
}

bool Analyzer::foldArrayStoreCheck(CallBase *CB) {
  // jeandle.array_store_check(value, array). §2.3.14 of the PEA paper
  // marks the op as read-only on the heap, so a virtual base by itself does
  // not constitute an escape. The fold compares the value's klass against
  // the array's element klass (via VMCallback ArrayElementKlass) and
  // either elides the call (provably compatible / primitive element) or
  // forces materialization (provably incompatible / element klass unknown
  // / value klass unknown).
  if (CB->arg_size() < 2)
    return true; // malformed; treat as non-escaping no-op
  auto ArrayID = jeandle::pea::resolveVirtualRef(CB->getArgOperand(1),
                                                 CurrentState, Aliases, DL);
  if (!ArrayID)
    return true;
  jeandle::VirtualObject &ArrayObj = *Result.VirtualObjects[*ArrayID];

  const jeandle::VMCallbacks *VMCB = jeandle::getVMCallbacks();
  if (!VMCB || !VMCB->ArrayElementKlass) {
    // No VMCallback — match prior behaviour: leave the call alone; the array
    // does not formally escape per §2.3.14.
    return true;
  }
  if (ArrayObj.Klass == 0)
    return true;

  uintptr_t ElementKlass = VMCB->ArrayElementKlass(ArrayObj.Klass);
  if (ElementKlass == 0) {
    // Primitive (type-array) element: array_store_check is a no-op on
    // primitive arrays. Unconditionally elide.
    Constant *True = ConstantInt::getTrue(CB->getType());
    emitReplaceCall(CB, True, *ArrayID);
    return true;
  }

  // Object[] element: compare the value's klass against the element klass.
  Value *Val = CB->getArgOperand(0);
  uintptr_t ValueKlass = 0;
  if (auto ValueID = jeandle::pea::resolveVirtualRef(Val, CurrentState,
                                                    Aliases, DL)) {
    // Virtual values carry an exact, concrete klass.
    ValueKlass = Result.VirtualObjects[*ValueID]->Klass;
  } else {
    // Fall back to attribute / metadata sharpening for non-virtual values.
    jeandle::JavaType JT = jeandle::getJavaType(Val);
    ValueKlass = JT.Klass;
  }

  if (ValueKlass == 0) {
    // Unknown value klass — cannot prove compatibility. Bail conservatively
    // by materializing the array so the surviving array_store_check sees a
    // real array pointer.
    Eligible[*ArrayID] = false;
    return true;
  }

  auto Folded = evalSubtypeRelation(ValueKlass, ElementKlass);
  if (!Folded) {
    // Could not prove subtype OR incompatibility (e.g., neither direction
    // of IsSubtype gave a definitive answer for a non-final element klass).
    // Bail conservatively.
    Eligible[*ArrayID] = false;
    return true;
  }
  if (*Folded) {
    // Provably compatible — elide.
    Constant *True = ConstantInt::getTrue(CB->getType());
    emitReplaceCall(CB, True, *ArrayID);
    return true;
  }
  // Provably incompatible: at runtime this would throw ArrayStoreException.
  // Materialize the array so the surviving call hits a real pointer.
  Eligible[*ArrayID] = false;
  return true;
}

bool Analyzer::foldCheckIfValueBased(CallBase *CB) {
  // jeandle.check_if_value_based(oop) -> i1.  Java emits this around
  // monitorenter on receivers whose static type could be a value-based class
  // (e.g. java.lang.Long), so that the runtime raises a
  // DiagnoseSyncOnValueBasedClasses warning when the dynamic klass is in fact
  // value-based.
  //
  // Fold logic: if the exact runtime class is known, the check collapses to
  // a compile-time constant. For a VIRTUAL receiver:
  //
  //   * VObj.Klass unknown (0)              -> bail (keep the call, virtual
  //                                            will materialize via the
  //                                            generic escape path).
  //   * IsValueBased(VObj.Klass) == true    -> force materialize. The runtime
  //                                            warning MUST observe a real
  //                                            oop; eliding it would change
  //                                            user-visible semantics.
  //   * IsValueBased(VObj.Klass) == false   -> elide the call (constant false).
  //                                            The virtual receiver is
  //                                            provably NOT value-based.
  if (CB->arg_size() < 1)
    return false;
  auto BaseID = jeandle::pea::resolveVirtualRef(CB->getArgOperand(0),
                                                CurrentState, Aliases, DL);
  if (!BaseID)
    return false;
  jeandle::VirtualObject &VObj = *Result.VirtualObjects[*BaseID];
  if (VObj.Klass == 0) {
    // Klass unknown — cannot prove either direction. Fall through to the
    // generic-escape path so the virtual gets materialized and the original
    // runtime check survives on a real oop.
    return false;
  }
  const jeandle::VMCallbacks *VMCB = jeandle::getVMCallbacks();
  if (!VMCB || !VMCB->IsValueBased) {
    // No callback registered (offline tests without a callback log). Bail
    // conservatively so the virtual materializes; matches the prior
    // pre-fold behaviour.
    return false;
  }
  if (VMCB->IsValueBased(VObj.Klass)) {
    // The dynamic klass IS value-based: HotSpot's runtime warning hook for
    // DiagnoseSyncOnValueBasedClasses must observe a real oop. Drop this
    // virtual's eligibility — commit() will discard every recorded effect
    // for it and the original allocation + check call stay in IR, where
    // the call ends up operating on the materialized pointer. Matches the
    // foldArrayStoreCheck "unknown value klass" conservative path.
    Eligible[*BaseID] = false;
    return true;
  }
  // Provably non-value-based: fold the check to false. The query against
  // an exact (virtualized) klass that does not carry the ValueBased marker
  // canonicalizes to false.
  Constant *False = ConstantInt::getFalse(CB->getType());
  emitReplaceCall(CB, False, *BaseID);
  return true;
}


bool Analyzer::foldRegisterFinalizerIfNeeded(CallBase *CB) {
  // jeandle.register_finalizer_if_needed(oop) -> void.
  //
  // The JavaOp's default lowering preserves HotSpot semantics by checking the
  // receiver klass finalizer bit and calling SharedRuntime_register_finalizer
  // only when needed. Finalizability is resolved at the allocation site: tier1
  // (new_instance handling) refuses to virtualize any instance whose exact
  // klass has a finalizer, so such an object stays materialized and this call
  // survives to be lowered normally. A virtual receiver reaching this fold is
  // therefore non-finalizable by construction — delete the provably-no-op
  // call without forcing the object header to materialize.
  if (CB->arg_size() < 1)
    return false;
  auto BaseID = jeandle::pea::resolveVirtualRef(CB->getArgOperand(0),
                                                CurrentState, Aliases, DL);
  if (!BaseID)
    return false;
  jeandle::VirtualObject &VObj = *Result.VirtualObjects[*BaseID];
  if (VObj.Klass == 0)
    return false;
  const jeandle::VMCallbacks *VMCB = jeandle::getVMCallbacks();
  if (!VMCB || !VMCB->HasFinalizer)
    return false;
  // A virtual receiver can only reach here if tier1 virtualized its
  // allocation, and tier1 (new_instance handling) refuses to virtualize any
  // instance whose exact klass has a finalizer. So by construction the
  // receiver is non-finalizable: the runtime check would always be false and
  // SharedRuntime_register_finalizer would never fire. Assert that invariant,
  // then delete the provably-no-op call so the allocation can be eliminated.
  assert(!VMCB->HasFinalizer(VObj.Klass) &&
         "tier1 must refuse finalizable klasses");
  emitReplaceCall(CB, nullptr, *BaseID);
  return true;
}

bool Analyzer::tier2JavaOpFold(CallBase *CB) {
  using namespace jeandle::pea;
  if (isJeandleArrayLength(CB))       return foldArrayLength(CB);
  if (isJeandleLoadKlass(CB))         return foldLoadKlass(CB);
  if (isJeandleCheckCast(CB))         return foldCheckCast(CB);
  if (isJeandleCheckInstanceOf(CB))   return foldCheckInstanceOf(CB);
  if (isJeandleInstanceOf(CB))        return foldInstanceOf(CB);
  if (isJeandleMonitorEnter(CB))      return foldMonitorEnter(CB);
  if (isJeandleMonitorExit(CB))       return foldMonitorExit(CB);
  if (isJeandleArrayStoreCheck(CB))   return foldArrayStoreCheck(CB);
  if (isJeandleCheckIfValueBased(CB)) return foldCheckIfValueBased(CB);
  if (isJeandleRegisterFinalizerIfNeeded(CB))
    return foldRegisterFinalizerIfNeeded(CB);
  return false;
}

void Analyzer::propagatePointerAlias(Instruction *I) {
  // The instruction is a pointer-identity-preserving transformation whose
  // operand carries a virtual alias. Forward the alias to the result.
  if (Aliases.getVirtualAlias(I))
    return;
  auto BaseID =
      jeandle::pea::resolveVirtualRef(I, CurrentState, Aliases, DL);
  if (!BaseID) {
    // Couldn't resolve — the underlying chain may have already escaped.
    materializeAllVirtualOperands(I);
    return;
  }
  Aliases.addVirtualAlias(I, *BaseID);
}

void Analyzer::materializeAllVirtualOperands(Instruction *I) {
  // Trigger materialization for every distinct virtual ObjectID that I uses.
  // After materializeAt, the per-object state in CurrentState flips to
  // Materialized so subsequent resolveVirtualRef queries return nullopt.
  // Operand-input rewriting is handled implicitly by the transform: it RAUWs
  // the original allocation with the materialized CallInst, so all IR uses
  // (including I's operand here) auto-update.
  SmallVector<jeandle::ObjectID, 4> ToMaterialize;
  DenseSet<jeandle::ObjectID> Seen;
  for (Use &U : I->operands()) {
    Value *V = U.get();
    if (!V)
      continue;
    if (auto MaybeID =
            jeandle::pea::resolveVirtualRef(V, CurrentState, Aliases, DL)) {
      if (Seen.insert(*MaybeID).second)
        ToMaterialize.push_back(*MaybeID);
    }
  }
  llvm::sort(ToMaterialize);
  for (jeandle::ObjectID ID : ToMaterialize)
    materializeAt(ID, I, MatReason::Unhandled);
}

// Compute the SAFE materialization point for an allocation: the earliest IR
// position immediately after the allocation completes. The new materialization
// invoke must be inserted here so that it dominates every existing use of the
// original allocation; otherwise the RAUW that snaps OrigAlloc onto the new
// invoke would replace earlier-in-block uses with a later-defined value, an
// SSA dominance violation that downstream passes (e.g. RewriteStatepointsForGC)
// reject.
//
// For an InvokeInst allocation, "immediately after" is the first non-PHI/dbg
// instruction in the normal-dest block. For a plain CallInst allocation, it's
// the instruction directly following the call. The returned instruction is
// guaranteed to dominate every use of the original allocation in the function.
static Instruction *computeMaterializationPoint(llvm::CallBase *Alloc) {
  if (auto *II = llvm::dyn_cast<llvm::InvokeInst>(Alloc)) {
    llvm::BasicBlock *Normal = II->getNormalDest();
    return &*Normal->getFirstNonPHIOrDbg();
  }
  return Alloc->getNextNode();
}

// Returns true iff CB has at least one "deopt" operand bundle.
static bool hasDeoptBundle(CallBase *CB) {
  for (unsigned i = 0, n = CB->getNumOperandBundles(); i < n; ++i)
    if (CB->getOperandBundleAt(i).getTagName() == "deopt")
      return true;
  return false;
}

// Sweep sibling VOs' FieldStates after a materialisation. Runs for EVERY
// materialisation — outer OR recursive — not just the recursive
// prerequisite path that the previous Jeandle code covered.
void Analyzer::updateOtherStatesForMaterialized(
    jeandle::ObjectID FlippedID, Value *NewPtr,
    DenseMap<jeandle::ObjectID, DenseMap<int64_t, jeandle::FieldValue>> &FS) {
  for (auto &OtherKv : FS) {
    if (OtherKv.first == FlippedID)
      continue;
    for (auto &Entry : OtherKv.second) {
      if (Entry.second.isVirtualRef() &&
          Entry.second.getVirtualRef() == FlippedID) {
        Entry.second = jeandle::FieldValue::materializedRef(NewPtr);
      }
    }
  }
}

void Analyzer::bumpMaterializeStat(MatReason R) {
  ++JeandlePEAMaterialized;
  switch (R) {
  case MatReason::Unhandled:
    ++JeandlePEAMaterializedUnhandled;
    break;
  case MatReason::Cascade:
    // Cascade and nested still count toward the Unhandled
    // rollup (they are byproducts of an upstream Unhandled escape) but
    // are also bookkept in their own counter for fine-grained audits.
    ++JeandlePEAMaterializedUnhandled;
    ++JeandlePEAMaterializedCascade;
    break;
  case MatReason::Nested:
    ++JeandlePEAMaterializedUnhandled;
    ++JeandlePEAMaterializedNested;
    break;
  case MatReason::Merge:
    ++JeandlePEAMaterializedMerge;
    break;
  case MatReason::LoopExit:
    ++JeandlePEAMaterializedLoopExit;
    break;
  case MatReason::PHI:
    ++JeandlePEAMaterializedPHI;
    break;
  case MatReason::Unknown:
    break;
  }
}

void Analyzer::materializeAt(jeandle::ObjectID ID,
                             Instruction *InsertBefore,
                             MatReason Reason) {
  if (Materialized.count(ID))
    return; // idempotent — first escape wins; also breaks nested-cycles.
  if (!Eligible.lookup(ID))
    return; // already gave up on this object; nothing to materialize.
  // Dead-block guard. The dead-edge sweep marks PEABlockState::Dead when a
  // block's only incoming edges are statically-dead constant-condition
  // branches. Materializing in a dead block is unsound: the IP we'd pick
  // is unreachable in practice, but the Effect would still be committed
  // by the transform, leaving stranded allocations the next
  // canonicalisation iteration has to clean up. Bail early so dead-
  // pred merges and dead-edge phi fan-ins do not produce phantom IR.
  if (CurrentState.isDead())
    return;

  jeandle::VirtualObject &VObj = *Result.VirtualObjects[ID];

  // Detect "inner MATERIALIZE_ALL touches outer VO" — set
  // OverflowFlag so the outer-most processLoop's body iteration breaks
  // out and runs the proper rollback + redo. We approximate "outer VO"
  // by checking whether the VO's allocation site lives OUTSIDE the
  // currently-processed loop. (Synthetic VOs have a borrowed
  // AllocationCall but their identity is per-merge so they always count
  // as inside the current loop; bail out of the check for them.)
  if (CurrentMode == Mode::MaterializeAll && CurrentProcessLoop &&
      VObj.AllocationCall && !VObj.IsSynthetic) {
    BasicBlock *AllocBB = VObj.AllocationCall->getParent();
    if (AllocBB && !CurrentProcessLoop->contains(AllocBB))
      OverflowFlag = true;
  }

  // PHI Case C synthetic VOs cannot be materialized. There is no
  // per-pred allocation to RAUW from — the new VO was conjured at the merge
  // out of per-pred VOs whose allocations live in different blocks. The
  // proper resolution (ensure-materialize each per-pred VO at its
  // end-of-block and thread the per-pred materialized pointers through
  // the existing PHI so it BECOMES the materialized pointer) is deferred;
  // we take the simpler conservative path:
  //   (a) mark the synthetic VO ineligible, so dropEffectsFor strips the
  //       per-entry field PHIs and the alias.
  //   (b) mark every per-pred source VO ineligible too — their allocations
  //       must survive in IR so the existing PHI's incoming values resolve
  //       to real pointers.
  //   (c) unregister the PHI's virtual alias so any subsequent
  //       resolveVirtualRef on the PHI returns nullopt and downstream
  //       consumers stop trying to fold through it.
  // The cascade-materialize path is deferred to a follow-on change.
  if (VObj.IsSynthetic) {
    Eligible[ID] = false;
    for (jeandle::ObjectID PID : VObj.SyntheticSourceIDs)
      Eligible[PID] = false;
    if (VObj.SyntheticPhi)
      Aliases.resetAlias(VObj.SyntheticPhi);
    // Mark Materialized so the idempotent guard at the top of subsequent
    // materializeAt calls short-circuits and we don't re-bail repeatedly.
    Materialized.insert(ID);
    return;
  }

  // Cycle prevention: insert into Materialized BEFORE recursing on any
  // nested VirtualRef ("flip the state then recurse"). This guarantees
  // that a self-referential or cyclic field graph (A.f = B, B.g = A)
  // terminates: the
  // recursive call short-circuits via the Materialized check at the top.
  // The cascade for strict-lock-order (below) also relies on this happening
  // before recursion so it doesn't re-enter the same ID.
  Materialized.insert(ID);

  // Lock cascade on materialization. When the object has live locks at
  // this point (LockCount > 0) AND the target's runtime requires strict
  // lock nesting (HotSpot LM_LIGHTWEIGHT), some other still-locked virtuals
  // must be materialized at the same insertion point so the runtime's
  // monitor-stack ordering remains intact.
  //
  // Narrow cascade rule:
  //   int lockDepth = obj.getMaximumLockDepth();
  //   for (VirtualObject other : ...)
  //     if (otherState.hasLocks() &&
  //         otherState.getMinimumLockDepth() < lockDepth)
  //       materialize(other, ...);
  //
  // We use the per-LockEnter Order tag as the lock-depth proxy: the
  // OUTERMOST live lock on a VO is at
  // LiveLockEnters[id].front().Order (min lock depth), and the INNERMOST
  // is at LiveLockEnters[id].back().Order (max lock depth). The rule
  // "other.min < this.max" thus selects exactly those VOs whose oldest
  // still-held lock predates this VO's newest still-held lock — i.e., VOs
  // that were locked BEFORE this VO's most recent enter. Materializing
  // them ensures their monitorenters are emitted FIRST (before this VO's),
  // so the real lock stack reproduces the original lexical order.
  //
  // VOs locked entirely AFTER this VO's max (other.min >= this.max) stay
  // virtual: their monitorenter/exit pair is fully contained within our
  // current scope and folds away cleanly without ever touching the real
  // lock stack.
  auto LCIt = LockCounts.find(ID);
  bool HasLiveLocks = (LCIt != LockCounts.end() && LCIt->second != 0);
  if (HasLiveLocks && StrictLockOrder) {
    auto ThisIt = LiveLockEnters.find(ID);
    assert(ThisIt != LiveLockEnters.end() && !ThisIt->second.empty() &&
           "LockCount > 0 implies a non-empty LiveLockEnters stack");
    // Cascade by BytecodeDepth. When !jeandle.lock_depth metadata is
    // present, this is the true bytecode monitor depth; otherwise
    // BytecodeDepth carries the Order proxy value at push time, so
    // behavior is bit-identical to the earlier implementation. This is
    // the depth-aware narrow rule (getMinimumLockDepth /
    // getMaximumLockDepth comparison).
    uint32_t ThisMaxDepth = ThisIt->second.back().BytecodeDepth;
    SmallVector<jeandle::ObjectID, 4> Cascade;
    for (auto &Kv : LiveLockEnters) {
      if (Kv.first == ID || Kv.second.empty())
        continue;
      if (Materialized.count(Kv.first))
        continue;
      // Narrow rule: only cascade VOs whose OUTERMOST live lock was
      // acquired strictly before this VO's INNERMOST live lock.
      if (Kv.second.front().BytecodeDepth < ThisMaxDepth)
        Cascade.push_back(Kv.first);
    }
    llvm::sort(Cascade); // deterministic order
    for (jeandle::ObjectID OtherID : Cascade)
      materializeAt(OtherID, InsertBefore, MatReason::Cascade);
  }

  // Undo monitor-call elisions for ID when there are live locks. For each
  // unbalanced enter on the live stack we (a) drop its previously-recorded
  // ReplaceCall(true) effect so the original call survives the transform,
  // and (b) emit a ReplaceInput effect that retargets the call's first
  // operand (the receiver) onto the materialized pointer at apply time. The
  // transform's NewAllocFor map redirects VObj.AllocationCall onto the live
  // materialized invoke. Matching exits downstream of the escape point are
  // not elided in the first place (foldMonitorExit on a Materialized object
  // returns false) and survive in IR with their operand RAUW'd.
  //
  // Balanced pairs whose exits we already saw on this path are NOT on the
  // live stack — their elision stays in place (better performance).
  auto SIt = LiveLockEnters.find(ID);
  if (HasLiveLocks && SIt != LiveLockEnters.end() && !SIt->second.empty()) {
    // Snapshot the live stack so we can clear it before recursing.
    SmallVector<LockEnter, 4> Stack(SIt->second.begin(), SIt->second.end());
    DenseSet<Instruction *> ToUndo;
    for (const LockEnter &LE : Stack)
      ToUndo.insert(LE.Call);

    // Walk every block's effect list and erase any ReplaceCall effect whose
    // target is one of the unbalanced enter call sites.
    for (auto &Kv : Result.BlockEffects) {
      auto &Effects = Kv.second;
      Effects.erase(
          std::remove_if(Effects.begin(), Effects.end(),
                         [&](const jeandle::PEAResult::Effect &E) {
                           return E.Kind ==
                                      jeandle::PEAResult::EffectKind::ReplaceCall &&
                                  ToUndo.count(E.Target);
                         }),
          Effects.end());
    }

    // Emit a ReplaceInput effect per enter, retargeting op0 onto the
    // materialized pointer (resolved via NewAllocFor at apply time).
    for (const LockEnter &LE : Stack) {
      CallBase *Call = LE.Call;
      jeandle::PEAResult::Effect E;
      E.Kind = jeandle::PEAResult::EffectKind::ReplaceInput;
      E.Block = Call->getParent();
      E.SeqNo = Result.nextSeqNo();
      E.Target = Call;
      E.InputIndex = 0;
      E.Replacement = VObj.AllocationCall;
      E.ObjID = ID;
      Result.addBlockEffect(std::move(E));
    }

    // The locks are now satisfied by real IR calls; clear the live stack
    // and the lock counter so commit()'s post-pass doesn't flip the object
    // back to ineligible.
    LockCounts[ID] = 0;
    LiveLockEnters.erase(ID);
    // Keep ObjectState::Locks in lockstep. NOTE: by this point the
    // ObjectState may already be Materialized (materialize() above clears
    // Entries but not Locks; we want Locks empty too). The state may also
    // still be Virtual on paths where materialize() was deferred — handle
    // both cases with the same clear.
    if (CurrentState.hasObjectState(ID)) {
      jeandle::ObjectState &OS = CurrentState.getObjectStateForModification(ID);
      if (OS.isVirtual())
        OS.clearLocks();
    }
  }

  // Recursive prerequisite materialization. For each field that holds a
  // VirtualRef to an inner virtual object, materialize the inner first, then
  // rewrite the outer's FieldStates entry to a MaterializedRef pointing at
  // the inner's *original* allocation. The transform substitutes that with
  // the new materialized invoke at apply time via NewAllocFor.
  {
    auto FSIt = FieldStates.find(ID);
    if (FSIt != FieldStates.end()) {
      SmallVector<int64_t, 4> NestedOffsets;
      for (auto &Kv : FSIt->second)
        if (Kv.second.isVirtualRef())
          NestedOffsets.push_back(Kv.first);
      llvm::sort(NestedOffsets); // determinism
      for (int64_t Off : NestedOffsets) {
        // Re-lookup each iteration; the recursion may have mutated FieldStates
        // (updateStatesForMaterialized below) and invalidated iterators.
        auto It2 = FieldStates.find(ID);
        if (It2 == FieldStates.end())
          break;
        auto OffIt = It2->second.find(Off);
        if (OffIt == It2->second.end() || !OffIt->second.isVirtualRef())
          continue;
        jeandle::ObjectID InnerID = OffIt->second.getVirtualRef();
        materializeAt(InnerID, InsertBefore, MatReason::Nested);
        if (!Eligible.lookup(InnerID)) {
          // Inner gave up — the outer cannot be materialized either, since
          // its replay would need a value inner can no longer produce.
          Eligible[ID] = false;
          return;
        }
        jeandle::VirtualObject &InnerVO = *Result.VirtualObjects[InnerID];
        FieldStates[ID][Off] =
            jeandle::FieldValue::materializedRef(InnerVO.AllocationCall);

        // updateStatesForMaterialized: every other still-tracked object whose
        // FieldStates references InnerID must also be flipped to
        // MaterializedRef so any later store/load on those objects sees the
        // materialized pointer.
        updateOtherStatesForMaterialized(InnerID, InnerVO.AllocationCall,
                                         FieldStates);
        // Drop alias-map entries that resolve to InnerID. The transform's
        // RAUW propagates the new pointer through the IR for downstream
        // consumers; we no longer want the analyzer to treat those Values as
        // virtual aliases of InnerID.
        SmallVector<Value *, 4> AliasesToDrop;
        for (auto &AKv : Aliases.virtualAliasesView()) {
          if (AKv.second == InnerID)
            AliasesToDrop.push_back(AKv.first);
        }
        for (Value *V : AliasesToDrop)
          Aliases.resetAlias(V);
      }
    }
  }

  // Soundness fix: rather than insert the materialization invoke at the
  // escape point (which would leave SSA-uses in the same block ahead of the
  // new definition once the transform RAUWs OrigAlloc onto it), hoist the
  // materialization back to the original allocation point. The chosen SafeIP
  // immediately follows OrigAlloc, so the new materialization invoke
  // dominates every existing use of OrigAlloc in the function.
  Instruction *SafeIP = computeMaterializationPoint(VObj.AllocationCall);
  assert(SafeIP && "alloc must have a follow-on instruction at SafeIP");

  // Per-field dominance check. After VirtualRef rewriting above, all entries
  // are Scalar or MaterializedRef. For MaterializedRef values produced by the
  // recursion (= an inner object's original allocation Instruction), the
  // dominance is trivially satisfied: the inner's allocation precedes the
  // store-into-outer in IR order, hence dominates outer's SafeIP.
  auto FSIt = FieldStates.find(ID);
  if (FSIt != FieldStates.end()) {
    for (auto &Kv : FSIt->second) {
      const jeandle::FieldValue &FV = Kv.second;
      Value *V = nullptr;
      if (FV.isScalar())
        V = FV.getScalar();
      else if (FV.isMaterializedRef())
        V = FV.getMaterialized();
      else
        continue;
      if (!V)
        continue;
      if (auto *VI = dyn_cast<Instruction>(V)) {
        if (!DT.dominates(VI, SafeIP)) {
          Eligible[ID] = false;
          return;
        }
      }
    }
  }

  // Snapshot tracked field values at this escape point. The transform replays
  // them as stores immediately after the new allocation call.
  jeandle::PEAResult::Effect E;
  E.Kind = jeandle::PEAResult::EffectKind::Materialize;
  E.Block = SafeIP->getParent();
  E.SeqNo = Result.nextSeqNo();
  E.InsertBefore = SafeIP;
  // Carry the original allocation as Target so the transform can RAUW it onto
  // the new materialized CallInst.
  E.Target = VObj.AllocationCall;
  E.ObjID = ID;

  // Prefer the escape-point CallBase as the operand-bundle source if
  // it carries a "deopt" bundle; otherwise fall back to the original
  // allocation. This honours the contract documented at
  // PartialEscape.h:506-509 ("escape-point instruction if it carries a deopt
  // bundle, otherwise the original allocation invoke") which the previous
  // unconditional assignment violated. Without this, a rich "deopt" bundle
  // attached to the escape-point sink call would be lost on the
  // materialization invoke and RewriteStatepointsForGC would emit a
  // statepoint with no deopt info.
  CallBase *DBS = VObj.AllocationCall;
  if (auto *CB = dyn_cast<CallBase>(InsertBefore))
    if (hasDeoptBundle(CB))
      DBS = CB;
  E.DeoptBundleSource = DBS;

  if (FSIt != FieldStates.end()) {
    // Deterministic ordering for the snapshot.
    SmallVector<int64_t, 8> Offsets;
    Offsets.reserve(FSIt->second.size());
    for (auto &Kv : FSIt->second)
      Offsets.push_back(Kv.first);
    llvm::sort(Offsets);
    for (int64_t Off : Offsets) {
      const jeandle::FieldValue &FV = FSIt->second.lookup(Off);
      if (FV.isUnknown())
        continue;
      E.FieldEntries.push_back({Off, FV});
    }
  }
  Result.addBlockEffect(std::move(E));
  // Bump per-reason statistics. Counted at emission time; a late
  // dropEffectsFor on this ID will leave the counter slightly inflated.
  bumpMaterializeStat(Reason);

  // Flip the per-object state so the rest of the analyzer treats the object
  // as already-materialized on this path. We hand the original allocation in
  // as a placeholder pointer; the transform RAUWs it onto the real
  // materialized CallInst before erasing it.
  CurrentState.getObjectStateForModification(ID).materialize(
      VObj.AllocationCall);

  // Sweep sibling VOs whose FieldStates still hold a VirtualRef to
  // this just-materialised outer object. Without this, a later store/load
  // through a sibling field would observe a stale VirtualRef(ID) entry and
  // either fire the PartialEscapeTransform debug assert (which insists every
  // VirtualRef must have been rewritten during analysis) or silently drop
  // the field. Runs for EVERY materialisation, not just the recursive
  // prerequisite path.
  updateOtherStatesForMaterialized(ID, VObj.AllocationCall, FieldStates);
}

void Analyzer::dropEffectsFor(jeandle::ObjectID ID) {
  bool DroppedAllocation = false;
  for (auto &Kv : Result.BlockEffects) {
    auto &Effects = Kv.second;
    Effects.erase(
        std::remove_if(Effects.begin(), Effects.end(),
                       [&](const jeandle::PEAResult::Effect &E) {
                         if (E.ObjID != ID)
                           return false;
                         if (E.Kind ==
                             jeandle::PEAResult::EffectKind::EliminateAllocation)
                           DroppedAllocation = true;
                         return true;
                       }),
        Effects.end());
  }
  if (DroppedAllocation) {
    --Result.VirtualizationDelta;
    ++Result.AllocationDelta;
  }
  Result.EscapeClassification[ID] =
      jeandle::PEAResult::EscapeKind::AlwaysEscapes;
}

void Analyzer::commit() {
  // Any virtual whose monitor lock count is non-zero at the end of analysis
  // has unbalanced enter/exit pairs (e.g., only an enter was seen, or the
  // matching exit lives in a different block we don't track yet). Mark
  // those ineligible up-front; the per-object loop below will drop their
  // effects.
  for (auto &Kv : LockCounts) {
    if (Kv.second != 0)
      Eligible[Kv.first] = false;
  }

  // Iterate by dense ObjectID order for determinism. The only remaining
  // post-pass cleanup is dropping effects for objects that became
  // ineligible during the walk (lock imbalance above; nested-virtual
  // discovery; tier-2 type mismatch / non-const offset; etc.). Cross-block
  // escapes no longer disqualify an object — they trigger materialization.
  for (auto &VObjUP : Result.VirtualObjects) {
    jeandle::ObjectID ID = VObjUP->getID();
    auto EIt = Eligible.find(ID);
    bool IsEligible = (EIt != Eligible.end()) && EIt->second;
    if (!IsEligible)
      dropEffectsFor(ID);
  }

  // -------------------------------------------------------------------------
  // EscapeClassification population.
  //
  // dropEffectsFor() already stamped AlwaysEscapes onto every ineligible VO.
  // For each surviving (eligible) VO, classify based on whether ANY
  // Materialize effect survived in the committed plan:
  //   * no Materialize  -> NeverEscapes      (alloc fully eliminated)
  //   * any Materialize -> PartiallyEscapes  (alloc eliminated on the
  //                                           virtual path, re-emitted on
  //                                           the escape path)
  // Maps to NEVER vs PARTIAL vs ALWAYS escape classification.
  // -------------------------------------------------------------------------
  DenseSet<jeandle::ObjectID> HasSurvivingMaterialize;
  unsigned SurvivingEliminations = 0;
  for (const auto &Kv : Result.BlockEffects) {
    for (const auto &E : Kv.second) {
      if (E.Kind == jeandle::PEAResult::EffectKind::Materialize)
        HasSurvivingMaterialize.insert(E.ObjID);
      else if (E.Kind == jeandle::PEAResult::EffectKind::EliminateAllocation)
        ++SurvivingEliminations;
    }
  }
  for (auto &VObjUP : Result.VirtualObjects) {
    jeandle::ObjectID ID = VObjUP->getID();
    // dropEffectsFor stamped AlwaysEscapes; skip those VOs.
    if (Result.EscapeClassification.count(ID))
      continue;
    Result.EscapeClassification[ID] =
        HasSurvivingMaterialize.count(ID)
            ? jeandle::PEAResult::EscapeKind::PartiallyEscapes
            : jeandle::PEAResult::EscapeKind::NeverEscapes;
  }

  // For every VO that ended up NeverEscapes (alloc will be
  // EliminateAllocation'd and its OrigAlloc users will RAUW to poison
  // in Pass 2), schedule the Case-B aliased PHIs for explicit erasure.
  // The transform's post-Pass-2 hook walks Result.CaseBAliasedPhisToErase
  // and runs RAUW(poison) + eraseFromParent on each surviving handle.
  // We deliberately do NOT schedule erasure for PartiallyEscapes or
  // AlwaysEscapes VOs: there, the PHI still routes a live materialized
  // pointer (or the OrigAlloc itself) and erasing would corrupt SSA.
  for (auto &Kv : CaseBPhiAliases) {
    jeandle::ObjectID ID = Kv.first;
    auto CIt = Result.EscapeClassification.find(ID);
    if (CIt == Result.EscapeClassification.end())
      continue;
    if (CIt->second != jeandle::PEAResult::EscapeKind::NeverEscapes)
      continue;
    for (PHINode *Phi : Kv.second) {
      if (!Phi)
        continue;
      Result.CaseBAliasedPhisToErase.emplace_back(Phi);
    }
  }
  // Count of EliminateAllocation effects that actually survived
  // commit() (i.e. allocations that will be removed by the transform).
  JeandlePEAEliminated += SurvivingEliminations;

  // -------------------------------------------------------------------------
  // -jeandle-dump-pea-stats summary line on stderr.
  // -------------------------------------------------------------------------
  if (JeandleDumpPEAStats) {
    unsigned NeverEsc = 0, PartialEsc = 0, AlwaysEsc = 0;
    for (const auto &Kv : Result.EscapeClassification) {
      switch (Kv.second) {
      case jeandle::PEAResult::EscapeKind::NeverEscapes:
        ++NeverEsc;
        break;
      case jeandle::PEAResult::EscapeKind::PartiallyEscapes:
        ++PartialEsc;
        break;
      case jeandle::PEAResult::EscapeKind::AlwaysEscapes:
        ++AlwaysEsc;
        break;
      }
    }
    llvm::errs() << ";; PEA stats @" << F.getName()
                 << ": NeverEscapes=" << NeverEsc
                 << " PartiallyEscapes=" << PartialEsc
                 << " AlwaysEscapes=" << AlwaysEsc << "\n";
  }
}

// Preorder (outer-first) collection of all loops reachable from L. Determinism
// follows LoopInfo::iterator / Loop::getSubLoops ordering, which mirrors the
// source-IR block order.
static void collectLoopsPreorder(Loop *L, SmallVectorImpl<Loop *> &Out) {
  Out.push_back(L);
  for (Loop *Sub : L->getSubLoops())
    collectLoopsPreorder(Sub, Out);
}

// Loop soundness: after the RPO walk has populated BlockExits but before
// commit(), force-materialize every virtual that is still virtual at any
// loop preheader's terminator. This makes loops trivially sound — no
// virtual survives the loop boundary — at the cost of giving up
// virtualization across loops. Combined with the tier1Allocate refusal of
// loop-body allocs, the analyzer never tracks an object across a back-edge.
//
// Important sequencing: this MUST run after the per-block analysis (we need
// BlockExits[preheader] to know what's still virtual on the way into the loop)
// and BEFORE commit() (so the Materialize effects we add are subject to the
// same eligibility filter that drops effects for objects we've decided to
// abandon).
void Analyzer::processLoopExit(Loop *L) {
  // Force materialisation at any loop-exit whose successor is an EH pad
  // (landingpad / catchpad / cleanuppad). Without this, a loop-internal
  // materialise that produced only per-pred MatPerBlock entries leaves
  // exit-edge LCSSA PHIs (and exception handlers reading them) wired to
  // stale OrigAlloc placeholders. The forced drain at the exit block
  // ensures every still-virtual VO has a real allocation by the time the
  // EH handler executes.
  //
  // Downstream "deopt-bundle" detection is deferred (Jeandle has no
  // deopt machinery yet). Landingpad-block detection alone covers the
  // immediate-EH path that motivated this code.
  SmallVector<BasicBlock *, 4> ExitingBBs;
  L->getExitingBlocks(ExitingBBs);
  for (BasicBlock *ExitingBB : ExitingBBs) {
    Instruction *Term = ExitingBB->getTerminator();
    if (!Term)
      continue;
    bool ExitsToEH = false;
    for (BasicBlock *Succ : successors(ExitingBB)) {
      if (L->contains(Succ))
        continue;
      // Detect a real catch-handler exit. Pure cleanup landingpads
      // (clauses==0, the C++/Java `resume` pattern) are NOT considered:
      // the cleanup handler only runs `resume`, never observes the VO,
      // and forcing a materialisation there would defeat virtualisation
      // for every loop-local alloc that happens to be the body of an
      // invoke (which is the canonical Jeandle shape — every alloc IS
      // an invoke).
      //
      // Downstream "deopt-bundle" detection is deferred (Jeandle has
      // no deopt). Only catchswitch / catchpad headers and landingpads
      // with at least one explicit clause qualify here.
      if (auto *CSI = dyn_cast<CatchSwitchInst>(&*Succ->getFirstNonPHIIt())) {
        (void)CSI;
        ExitsToEH = true;
        break;
      }
      if (isa<CatchPadInst>(&*Succ->getFirstNonPHIIt())) {
        ExitsToEH = true;
        break;
      }
      if (auto *LP = dyn_cast<LandingPadInst>(&*Succ->getFirstNonPHIIt())) {
        if (LP->getNumClauses() > 0) {
          ExitsToEH = true;
          break;
        }
      }
    }
    if (!ExitsToEH)
      continue;
    auto It = BlockExits.find(ExitingBB);
    if (It == BlockExits.end())
      continue;
    BlockExitInfo &Exit = It->second;
    SmallVector<jeandle::ObjectID, 4> Vs(Exit.Virtuals.begin(),
                                          Exit.Virtuals.end());
    llvm::sort(Vs);
    for (jeandle::ObjectID ID : Vs) {
      if (!Eligible.lookup(ID))
        continue;
      // Materialise at the exiting block's terminator. The IP is the
      // last legal point inside the loop body; the EH pad inherits a
      // fully-materialised state.
      materializeAtPredFromExitInfo(ID, ExitingBB, Exit,
                                    /*SkipGlobalRAUW=*/false,
                                    MatReason::LoopExit);
    }
  }
}

void Analyzer::processStateBeforeLoopOnOverflow(Loop *L) {
  // Every VO still virtual at the loop's forward end (preheader exit) is
  // forcibly materialised, so the re-do of the loop body in
  // MATERIALIZE_ALL mode starts from a clean "no live virtuals on entry"
  // state.
  BasicBlock *PH = L->getLoopPreheader();
  if (!PH)
    return;
  auto It = BlockExits.find(PH);
  if (It == BlockExits.end())
    return;
  BlockExitInfo &PHExit = It->second;
  SmallVector<jeandle::ObjectID, 4> Vs(PHExit.Virtuals.begin(),
                                        PHExit.Virtuals.end());
  llvm::sort(Vs);
  for (jeandle::ObjectID ID : Vs) {
    if (!Eligible.lookup(ID))
      continue;
    materializeAtPredFromExitInfo(ID, PH, PHExit, /*SkipGlobalRAUW=*/false,
                                  MatReason::LoopExit);
  }
}

void Analyzer::materializePreheaderVirtualsForUnvisitedLoops() {
  SmallVector<Loop *, 8> AllLoops;
  for (Loop *L : LI)
    collectLoopsPreorder(L, AllLoops);

  for (Loop *L : AllLoops) {
    BasicBlock *PH = L->getLoopPreheader();
    if (!PH) {
      // Loops without a unique preheader are drained in-place by
      // processLoop (it materialises every still-virtual VO at every
      // forward header predecessor and marks the loop in PessimisticLoops
      // so the VisitedLoops gate below short-circuits). This branch is
      // a defense-in-depth no-op: the safety net cannot pick a single
      // PH to drain at when none exists, so the only sound action here
      // is to skip.
      continue;
    }
    // Strict gate on VisitedLoops. Every loop processLoop
    // touched — whether it converged (ConvergedLoops), fell into the
    // pessimistic MATERIALIZE_ALL fallback (PessimisticLoops), or hit
    // the overflow-recovery retry path — already had its
    // preheader virtuals handled inside processLoop. The only loops
    // that need this safety-net drain are those processLoop never
    // visited (an unreachable top-level loop the RPO walk skipped, or
    // a sub-loop whose outer recursion returned early on OverflowFlag).
    if (VisitedLoops.count(L))
      continue;
    auto It = BlockExits.find(PH);
    if (It == BlockExits.end())
      continue;
    BlockExitInfo &PHExit = It->second;

    // Snapshot+sort the virtual IDs to materialize. Sorting by ObjectID gives
    // deterministic effect ordering across runs.
    SmallVector<jeandle::ObjectID, 4> Vs(PHExit.Virtuals.begin(),
                                          PHExit.Virtuals.end());
    llvm::sort(Vs);
    for (jeandle::ObjectID ID : Vs) {
      if (!Eligible.lookup(ID))
        continue;
      materializeAtPredFromExitInfo(ID, PH, PHExit, /*SkipGlobalRAUW=*/false,
                                    MatReason::LoopExit);
    }
  }
}

// Like materializeAt, but operates against a pred's BlockExitInfo snapshot
// rather than the analyzer's current per-block state (which has moved on by
// the time materializeBeforeLoops runs). The function-wide MaterializedAtPred
// map dedups (and breaks cycles between) recursive nested-virtual
// materializations within a single PH and across multiple call sites
// (e.g. a mixed-state merge and a loop-preheader sweep at the same PH).
void Analyzer::materializeAtPredFromExitInfo(
    jeandle::ObjectID ID, BasicBlock *PH, BlockExitData &ExitInfo,
    bool SkipGlobalRAUW, MatReason Reason) {
  auto &MatInPH = MaterializedAtPred[PH];
  if (MatInPH.count(ID))
    return;
  if (!Eligible.lookup(ID))
    return;
  // Same dead-block guard as materializeAt. CurrentState is the
  // active per-block state; if the dead-edge sweep marked it Dead, every materialise
  // emitted from this point would pin IR onto an unreachable path.
  if (CurrentState.isDead())
    return;

  jeandle::VirtualObject &VObj = *Result.VirtualObjects[ID];

  // Same overflow check as materializeAt. If an inner-loop
  // MATERIALIZE_ALL drains a preheader-virtual that was allocated outside
  // the currently-processed loop, latch OverflowFlag so the outer
  // processLoop unwinds and re-runs.
  if (CurrentMode == Mode::MaterializeAll && CurrentProcessLoop &&
      VObj.AllocationCall && !VObj.IsSynthetic) {
    BasicBlock *AllocBB = VObj.AllocationCall->getParent();
    if (AllocBB && !CurrentProcessLoop->contains(AllocBB))
      OverflowFlag = true;
  }

  // Synthetic-VO bail (mirrors materializeAt). A synthetic VO created by
  // Case C has no backing per-pred allocation; its AllocationCall is shared
  // with one of the source VOs and using it as a Materialize target would
  // emit a re-allocation at PH that RAUWs the source alloc, breaking SSA
  // dominance at the Case-C merge PHI. The conservative strategy is
  // to drop the synthetic AND every per-pred source so the original
  // allocations and stores survive in IR. The cascade-materialize path
  // (ensure-materialize each per-pred source + reuse the existing PHI as
  // the materialized pointer) is deferred.
  if (VObj.IsSynthetic) {
    Eligible[ID] = false;
    for (jeandle::ObjectID PID : VObj.SyntheticSourceIDs)
      Eligible[PID] = false;
    if (VObj.SyntheticPhi)
      Aliases.resetAlias(VObj.SyntheticPhi);
    // Insert into MatInPH so any subsequent call short-circuits at the top.
    MatInPH.insert(ID);
    return;
  }

  // Cycle prevention before recursion (mirrors materializeAt).
  MatInPH.insert(ID);

  // Strict-lock-order cascade against this pred's snapshot. Narrow rule
  // — only cascade VOs whose outermost live lock (front().Order) was
  // acquired strictly before this VO's innermost live lock (back().Order).
  // Mirrors materializeAt's cascade above; see comment there.
  auto LCIt = ExitInfo.LockCounts.find(ID);
  bool HasLiveLocks = (LCIt != ExitInfo.LockCounts.end() && LCIt->second != 0);
  if (HasLiveLocks && StrictLockOrder) {
    auto ThisIt = ExitInfo.LiveLockEnters.find(ID);
    assert(ThisIt != ExitInfo.LiveLockEnters.end() &&
           !ThisIt->second.empty() &&
           "LockCount > 0 implies a non-empty LiveLockEnters stack");
    // Cascade by BytecodeDepth. See the matching comment in
    // materializeAt above for the rationale.
    uint32_t ThisMaxDepth = ThisIt->second.back().BytecodeDepth;
    SmallVector<jeandle::ObjectID, 4> Cascade;
    for (auto &Kv : ExitInfo.LiveLockEnters) {
      if (Kv.first == ID || Kv.second.empty())
        continue;
      if (MatInPH.count(Kv.first))
        continue;
      if (Kv.second.front().BytecodeDepth < ThisMaxDepth)
        Cascade.push_back(Kv.first);
    }
    llvm::sort(Cascade);
    for (jeandle::ObjectID OtherID : Cascade)
      materializeAtPredFromExitInfo(OtherID, PH, ExitInfo, SkipGlobalRAUW);
  }

  // Undo monitor-call elisions for ID when there are live locks. Reads the
  // snapshot's LiveLockEnters (the unbalanced enter call sites along the
  // path that produced ExitInfo). Mirrors materializeAt's logic.
  //
  // Two-phase emission (per-pred-safe):
  //   Phase 1 (here): drop the ReplaceCall(true) elision so the original
  //     enter calls survive the transform; clear the live lock state in both
  //     CurrentState and ExitInfo.
  //   Phase 2 (after Materialize emission below): emit a ReplaceInput effect
  //     per surviving enter call, tagged with E.Block = PH so the transform
  //     resolves the receiver via MatPerBlock[{PH, OrigAlloc}] (the PH's own
  //     materialized invoke) rather than the global NewAllocFor (which
  //     last-write-wins across per-pred materializations of the same
  //     OrigAlloc, breaking SSA when multiple preds materialize the same VO
  //     — the lock-mismatch case is the canonical example).
  SmallVector<LockEnter, 4> StackToUnElide;
  auto SIt = ExitInfo.LiveLockEnters.find(ID);
  if (HasLiveLocks && SIt != ExitInfo.LiveLockEnters.end() &&
      !SIt->second.empty()) {
    StackToUnElide.assign(SIt->second.begin(), SIt->second.end());
    DenseSet<Instruction *> ToUndo;
    for (const LockEnter &LE : StackToUnElide)
      ToUndo.insert(LE.Call);
    for (auto &Kv : Result.BlockEffects) {
      auto &Effects = Kv.second;
      Effects.erase(
          std::remove_if(Effects.begin(), Effects.end(),
                         [&](const jeandle::PEAResult::Effect &E) {
                           return E.Kind ==
                                      jeandle::PEAResult::EffectKind::ReplaceCall &&
                                  ToUndo.count(E.Target);
                         }),
          Effects.end());
    }
    // Clear locks and stack in both the live state (for commit()) and the
    // snapshot (for any subsequent merge that reads from this pred). The
    // matching ReplaceInput effects are emitted after the Materialize below.
    LockCounts[ID] = 0;
    LiveLockEnters.erase(ID);
    ExitInfo.LockCounts.erase(ID);
    ExitInfo.LiveLockEnters.erase(ID);
  }

  // Recursive prerequisite materialization, mirroring materializeAt but
  // walking ExitInfo.FieldStates instead of the analyzer's live FieldStates.
  {
    auto FSIt = ExitInfo.FieldStates.find(ID);
    if (FSIt != ExitInfo.FieldStates.end()) {
      SmallVector<int64_t, 4> NestedOffsets;
      for (auto &Kv : FSIt->second)
        if (Kv.second.isVirtualRef())
          NestedOffsets.push_back(Kv.first);
      llvm::sort(NestedOffsets);
      for (int64_t Off : NestedOffsets) {
        auto It2 = ExitInfo.FieldStates.find(ID);
        if (It2 == ExitInfo.FieldStates.end())
          break;
        auto OffIt = It2->second.find(Off);
        if (OffIt == It2->second.end() || !OffIt->second.isVirtualRef())
          continue;
        jeandle::ObjectID InnerID = OffIt->second.getVirtualRef();
        materializeAtPredFromExitInfo(InnerID, PH, ExitInfo, SkipGlobalRAUW);
        if (!Eligible.lookup(InnerID)) {
          Eligible[ID] = false;
          return;
        }
        jeandle::VirtualObject &InnerVO = *Result.VirtualObjects[InnerID];
        ExitInfo.FieldStates[ID][Off] =
            jeandle::FieldValue::materializedRef(InnerVO.AllocationCall);
        // updateStatesForMaterialized in the snapshot.
        updateOtherStatesForMaterialized(InnerID, InnerVO.AllocationCall,
                                         ExitInfo.FieldStates);
      }
    }
  }

  // Soundness fix (mirrors materializeAt).
  Instruction *SafeIP = &*PH->getFirstNonPHIOrDbg();
  Instruction *AllocSafe = computeMaterializationPoint(VObj.AllocationCall);
  if (AllocSafe && AllocSafe->getParent() == PH &&
      !DT.dominates(AllocSafe, SafeIP)) {
    SafeIP = AllocSafe;
  }

  // Per-field dominance check.
  auto FSIt = ExitInfo.FieldStates.find(ID);
  if (FSIt != ExitInfo.FieldStates.end()) {
    for (auto &Kv : FSIt->second) {
      const jeandle::FieldValue &FV = Kv.second;
      Value *V = nullptr;
      if (FV.isScalar())
        V = FV.getScalar();
      else if (FV.isMaterializedRef())
        V = FV.getMaterialized();
      else
        continue;
      if (!V)
        continue;
      if (auto *VI = dyn_cast<Instruction>(V)) {
        if (!DT.dominates(VI, SafeIP)) {
          Eligible[ID] = false;
          return;
        }
      }
    }
  }

  jeandle::PEAResult::Effect E;
  E.Kind = jeandle::PEAResult::EffectKind::Materialize;
  E.Block = SafeIP->getParent();
  E.SeqNo = Result.nextSeqNo();
  E.InsertBefore = SafeIP;
  E.Target = VObj.AllocationCall;
  E.ObjID = ID;
  E.DeoptBundleSource = VObj.AllocationCall;
  E.IsPerPred = SkipGlobalRAUW;

  if (FSIt != ExitInfo.FieldStates.end()) {
    SmallVector<int64_t, 8> Offsets;
    Offsets.reserve(FSIt->second.size());
    for (auto &Kv : FSIt->second)
      Offsets.push_back(Kv.first);
    llvm::sort(Offsets);
    for (int64_t Off : Offsets) {
      const jeandle::FieldValue &FV = FSIt->second.lookup(Off);
      if (FV.isUnknown())
        continue;
      E.FieldEntries.push_back({Off, FV});
    }
  }
  Result.addBlockEffect(std::move(E));
  // Bump per-reason statistics for the per-pred Materialize.
  bumpMaterializeStat(Reason);

  // Phase 2 of the un-elide: emit ReplaceInput effects in PH's bucket with
  // a higher SeqNo than the Materialize above, so within BlockEffects[PH]
  // the Materialize runs first (recording MatPerBlock[{PH, OrigAlloc}] =
  // NewInvAtPH) and the ReplaceInput resolves through that map to the
  // PH-specific NewInv. This is critical when multiple per-pred materializes
  // of the same OrigAlloc coexist (lock-mismatch case): the global
  // NewAllocFor map only holds the LAST per-pred NewInv, but each enter
  // call's operand must point to its OWN pred's NewInv.
  for (const LockEnter &LE : StackToUnElide) {
    CallBase *Call = LE.Call;
    jeandle::PEAResult::Effect RE;
    RE.Kind = jeandle::PEAResult::EffectKind::ReplaceInput;
    // Tag the effect with PH so the transform resolves via MatPerBlock.
    RE.Block = PH;
    RE.SeqNo = Result.nextSeqNo();
    RE.Target = Call;
    RE.InputIndex = 0;
    RE.Replacement = VObj.AllocationCall;
    RE.ObjID = ID;
    Result.addBlockEffect(std::move(RE));
  }

  // Post-materialize, flip the snapshot's per-object state so any later
  // merge that reads from this pred's ExitInfo sees the object as
  // materialized. The placeholder MaterializedValue is OrigAlloc; the
  // transform's MatPerBlock substitutes the live NewInv at apply time.
  ExitInfo.Virtuals.erase(ID);
  ExitInfo.Materialized.insert(ID);
  ExitInfo.MaterializedValues[ID] = VObj.AllocationCall;
  ExitInfo.FieldStates.erase(ID);
  ExitInfo.LockCounts.erase(ID);

  // Sweep sibling VOs in this pred's snapshot for stale
  // VirtualRef(ID) entries — same rationale as in materializeAt's outer-flip
  // sweep, but operating on the ExitInfo snapshot rather than the live
  // FieldStates. The ExitInfo's own FieldStates[ID] was just erased, so we
  // only need to walk the OTHER per-object entries.
  updateOtherStatesForMaterialized(ID, VObj.AllocationCall,
                                   ExitInfo.FieldStates);
}

// ===========================================================================
// Real loop fixpoint
// ===========================================================================
//
// Loop fixpoint:
//
//   * Iterate the loop body up to kMaxLoopFixpointIters times, each
//     iteration starting from a clean rollback to the pre-loop snapshot.
//   * The fixpoint variable is the per-block exit state of every block in
//     the loop. mergeStates(Header) on iteration i+1 sees both the
//     (unchanging) preheader BlockExitInfo and the iteration-i backedge
//     BlockExitInfo, so field PHIs / lock-merge / virtuality decisions
//     stabilise once the per-block exits stabilise.
//   * Field PHIs synthesised at the loop header MUST be stable across
//     iterations or the comparison-by-Value-pointer never agrees. That is
//     the entire purpose of LoopFieldPhiCache / OwnedLoopFieldPhis.
//   * On non-convergence, fall back to MATERIALIZE_ALL: rollback,
//     materialize every preheader-live virtual, then process the body
//     once in MATERIALIZE_ALL mode so any new allocations inside the
//     body are also refused.

// Equality on BlockExitData (the per-object base data). Two payloads are
// equivalent iff every per-object dimension (still-virtual set, materialized
// set, FieldStates per offset, LockCounts, LiveLockEnters,
// MaterializedValues) matches by structural comparison.
bool Analyzer::exitDataEquivalent(const BlockExitData &A,
                                  const BlockExitData &B) {
  if (A.Virtuals.size() != B.Virtuals.size())
    return false;
  for (auto ID : A.Virtuals)
    if (!B.Virtuals.count(ID))
      return false;
  if (A.Materialized.size() != B.Materialized.size())
    return false;
  for (auto ID : A.Materialized)
    if (!B.Materialized.count(ID))
      return false;
  if (A.FieldStates.size() != B.FieldStates.size())
    return false;
  for (auto &Kv : A.FieldStates) {
    auto It = B.FieldStates.find(Kv.first);
    if (It == B.FieldStates.end())
      return false;
    if (Kv.second.size() != It->second.size())
      return false;
    for (auto &Off : Kv.second) {
      auto OIt = It->second.find(Off.first);
      if (OIt == It->second.end())
        return false;
      if (!Off.second.shallowEquals(OIt->second))
        return false;
    }
  }
  if (A.LockCounts.size() != B.LockCounts.size())
    return false;
  for (auto &Kv : A.LockCounts) {
    auto It = B.LockCounts.find(Kv.first);
    if (It == B.LockCounts.end() || It->second != Kv.second)
      return false;
  }
  if (A.LiveLockEnters.size() != B.LiveLockEnters.size())
    return false;
  for (auto &Kv : A.LiveLockEnters) {
    auto It = B.LiveLockEnters.find(Kv.first);
    if (It == B.LiveLockEnters.end())
      return false;
    if (Kv.second.size() != It->second.size())
      return false;
    for (size_t i = 0, e = Kv.second.size(); i != e; ++i)
      // Loop fixpoint convergence: only the call-site identity matters; the
      // Order tag is monotonically refreshed on re-pushes and never settles,
      // so comparing it would cause loops with virtual monitorenters to
      // diverge forever.
      if (Kv.second[i].Call != It->second[i].Call)
        return false;
  }
  if (A.MaterializedValues.size() != B.MaterializedValues.size())
    return false;
  for (auto &Kv : A.MaterializedValues) {
    auto It = B.MaterializedValues.find(Kv.first);
    if (It == B.MaterializedValues.end() || It->second != Kv.second)
      return false;
  }
  return true;
}

// Equality on BlockExitInfo. Compares the per-object base data AND the
// state-split fields (TerminatorInvoke / UnwindDest / UnwindEdgeKilled /
// optional UnwindData snapshot). The exception-edge fields participate in the loop
// fixpoint convergence check so that a state-split appearing or
// disappearing across iterations is correctly observed as a change.
bool Analyzer::blockExitInfoEquivalent(const BlockExitInfo &A,
                                       const BlockExitInfo &B) {
  if (!exitDataEquivalent(A, B))
    return false;
  if (A.TerminatorInvoke != B.TerminatorInvoke)
    return false;
  if (A.UnwindDest != B.UnwindDest)
    return false;
  if (A.UnwindEdgeKilled != B.UnwindEdgeKilled)
    return false;
  const bool HasA = A.UnwindData.has_value();
  const bool HasB = B.UnwindData.has_value();
  if (HasA != HasB)
    return false;
  if (HasA && !exitDataEquivalent(*A.UnwindData, *B.UnwindData))
    return false;
  return true;
}

bool Analyzer::loopBlockExitsEquivalent(
    const llvm::SmallPtrSetImpl<BasicBlock *> &LoopBlocks,
    const DenseMap<BasicBlock *, BlockExitInfo> &A,
    const DenseMap<BasicBlock *, BlockExitInfo> &B) const {
  for (BasicBlock *BB : LoopBlocks) {
    auto AIt = A.find(BB);
    auto BIt = B.find(BB);
    bool HasA = (AIt != A.end());
    bool HasB = (BIt != B.end());
    if (HasA != HasB) {
      // Monotonicity guard. If this BB is known-alive from a prior
      // iteration but is missing from one of A/B, the fixpoint has dropped
      // exit state for a previously-live block. In debug builds, treat
      // this as a hard invariant violation so the regression surfaces at
      // the iteration boundary (instead of silently looping until the
      // iteration cap escalates to MATERIALIZE_ALL). In release builds we
      // treat the BB as equivalent and continue — a benign over-approxim-
      // ation that lets the rest of the structural comparison drive the
      // verdict (knownAliveLoopEnds invariant tolerance for this edge
      // case).
      if (KnownAliveLoopEnds.lookup(BB)) {
#ifndef NDEBUG
        llvm_unreachable("PEA loop monotonicity violation: known-alive loop "
                         "block dropped exit state across iterations");
#else
        continue;
#endif
      }
      return false;
    }
    if (!HasA)
      continue;
    if (!blockExitInfoEquivalent(AIt->second, BIt->second))
      return false;
  }
  return true;
}

void Analyzer::takeLoopSnapshot(
    Loop *L, const llvm::SmallPtrSetImpl<BasicBlock *> &LoopBlocks,
    LoopSnapshot &S) {
  S.CurrentState = CurrentState;
  S.Aliases = Aliases.snapshot();
  S.FieldStates = FieldStates;
  S.LockCounts = LockCounts;
  S.LiveLockEnters = LiveLockEnters;
  S.Materialized = Materialized;
  // Snapshot per-VO eligibility so a transient bail in this iter does
  // not wedge the VO ineligible for iter N+1.
  S.EligibleSnapshot = Eligible;
  // Also snapshot the VO count, so on restore we can re-mark every
  // VO created inside this iter as eligible for the next iter's retry.
  S.PreIterVOCount = Result.VirtualObjects.size();
  S.NextSeqNo = Result.NextSeqNo;
  S.OwnedPhisSize = Result.OwnedPhis.size();
  S.OwnedInstsSize = Result.OwnedInsts.size();
  S.SavedBlockExits.clear();
  S.SavedBlockEffects.clear();
  S.SavedMaterializedAtPred.clear();
  S.HadBlockExits.clear();
  S.HadBlockEffects.clear();
  S.HadMaterializedAtPred.clear();
  for (BasicBlock *BB : LoopBlocks) {
    auto EIt = BlockExits.find(BB);
    if (EIt != BlockExits.end()) {
      S.HadBlockExits.insert(BB);
      S.SavedBlockExits[BB] = EIt->second;
    }
    auto FIt = Result.BlockEffects.find(BB);
    if (FIt != Result.BlockEffects.end()) {
      S.HadBlockEffects.insert(BB);
      S.SavedBlockEffects[BB] = FIt->second;
    }
    auto MIt = MaterializedAtPred.find(BB);
    if (MIt != MaterializedAtPred.end()) {
      S.HadMaterializedAtPred.insert(BB);
      S.SavedMaterializedAtPred[BB] = MIt->second;
    }
  }
}

void Analyzer::restoreLoopSnapshot(
    const llvm::SmallPtrSetImpl<BasicBlock *> &LoopBlocks,
    const LoopSnapshot &S) {
  CurrentState = S.CurrentState;
  Aliases.restore(S.Aliases);
  FieldStates = S.FieldStates;
  LockCounts = S.LockCounts;
  LiveLockEnters = S.LiveLockEnters;
  Materialized = S.Materialized;
  // Restore Eligible map in full so transient per-iter bails do not
  // outlive the iteration they occurred in. Then re-mark every VO created
  // AFTER the snapshot was taken (i.e. allocs inside this iter's body) as
  // eligible — they survive in IR (we never erase invokes during analysis)
  // and the next iter's tier1Allocate hits AllocSiteToVO and would bail
  // early on `!Eligible.lookup(ID)` if we didn't re-prime it. Without
  // these post-snapshot re-marks, body-local allocations would be wedged
  // ineligible across the entire fixpoint after iter 0 — observable as
  // failure to virtualise a loop-local "new" with no escape.
  Eligible = S.EligibleSnapshot;
  for (size_t I = S.PreIterVOCount, E = Result.VirtualObjects.size(); I < E;
       ++I) {
    if (!Result.VirtualObjects[I])
      continue;
    Eligible[Result.VirtualObjects[I]->getID()] = true;
  }

  // Pop and delete unparented PHIs / insts created during the rolled-back
  // iteration. OwnedLoopFieldPhis are NOT touched — they're the per-loop
  // PHI cache, and the whole point of the cache is to keep them alive
  // across iterations.
  while (Result.OwnedPhis.size() > S.OwnedPhisSize) {
    WeakTrackingVH &VH = Result.OwnedPhis.back();
    if (Value *V = VH) {
      if (auto *P = dyn_cast<PHINode>(V))
        if (!P->getParent())
          delete P;
    }
    Result.OwnedPhis.pop_back();
  }
  while (Result.OwnedInsts.size() > S.OwnedInstsSize) {
    WeakTrackingVH &VH = Result.OwnedInsts.back();
    if (Value *V = VH) {
      if (auto *I = dyn_cast<Instruction>(V))
        if (!I->getParent())
          I->deleteValue();
    }
    Result.OwnedInsts.pop_back();
  }
  Result.NextSeqNo = S.NextSeqNo;

  // Roll back per-loop-block ledgers.
  //
  // Back-edge propagation: we deliberately DO NOT roll back BlockExits
  // for loop blocks. The next iteration's mergeStates(Header) reads each
  // back-edge pred's BlockExits to learn the loop-internal contribution; if
  // we wiped it here, mergeStates would only ever see the preheader (the
  // back-edge pred is later in RPO than the header), the loop-header field
  // PHI would never be synthesized, and the body's loop-carried state would
  // be silently lost. Leaving the iter-N BlockExits in place feeds iter
  // (N+1)'s header merge with the prior body exit; each block's
  // snapshotExitState() at the end of processBlock then overwrites the
  // stale entry with the iteration's fresh result, so by the end of iter
  // (N+1) every loop block's BlockExits reflects the new iteration alone.
  // The convergence check in processLoop compares iter (N+1)'s CurExits
  // against iter N's LastExits, so a never-changing BlockExits will still
  // converge after one extra iteration.
  //
  // The other ledgers (BlockEffects, MaterializedAtPred) MUST be rolled
  // back: they accumulate emitted-effect side-data, and leaving them would
  // duplicate Materialize / CreatePHI / ReplaceLoad effects across
  // iterations. The LoopFieldPhiCache (and Result.OwnedLoopFieldPhis)
  // covers the "stable PHI Value*" need for iter-spanning structural
  // equivalence on FieldStates.
  for (BasicBlock *BB : LoopBlocks) {
    auto SF = S.SavedBlockEffects.find(BB);
    if (S.HadBlockEffects.count(BB))
      Result.BlockEffects[BB] = SF->second;
    else
      Result.BlockEffects.erase(BB);
    auto SM = S.SavedMaterializedAtPred.find(BB);
    if (S.HadMaterializedAtPred.count(BB))
      MaterializedAtPred[BB] = SM->second;
    else
      MaterializedAtPred.erase(BB);
  }
}

void Analyzer::processLoopBodyOnePass(
    Loop *L, llvm::SmallPtrSetImpl<BasicBlock *> &OuterDone) {
  // Process loop blocks in function-RPO restricted to L. Sub-loop headers
  // dispatch recursively to processLoop, and the sub-loop's blocks are
  // marked Done so we don't re-process them in this pass.
  llvm::SmallPtrSet<BasicBlock *, 16> Done;
  ReversePostOrderTraversal<Function *> RPOT(&F);
  for (BasicBlock *BB : RPOT) {
    if (!L->contains(BB))
      continue;
    if (Done.count(BB))
      continue;
    Loop *Inner = LI.getLoopFor(BB);
    if (Inner && Inner != L && Inner->getHeader() == BB) {
      // Found a sub-loop's header — recurse.
      processLoop(Inner);
      for (BasicBlock *SB : Inner->blocks()) {
        Done.insert(SB);
        OuterDone.insert(SB);
      }
      continue;
    }
    processBlock(BB);
    Done.insert(BB);
    OuterDone.insert(BB);
  }
}

void Analyzer::processLoop(Loop *L) {
  // Record EVERY processLoop entry (including for loops with no
  // header / no preheader / OverflowFlag short-circuit) so the safety-net
  // pass treats this loop as visited and skips it.
  VisitedLoops.insert(L);
  BasicBlock *Preheader = L->getLoopPreheader();
  BasicBlock *Header = L->getHeader();
  if (!Header) {
    // Defensive: a loop with no header would be malformed LoopInfo state.
    return;
  }
  // Push/pop CurrentProcessLoop so materializeAt can detect when
  // an inner iteration's MATERIALIZE_ALL touches an OUTER VO.
  Loop *SavedProcessLoop = CurrentProcessLoop;
  CurrentProcessLoop = L;
  struct PopLoop {
    Analyzer *A;
    Loop *Saved;
    ~PopLoop() { A->CurrentProcessLoop = Saved; }
  } _pop{this, SavedProcessLoop};
  if (!Preheader) {
    // Loop without a unique preheader. Jeandle now schedules
    // LoopSimplifyPass before PEA, so natural loops on a reducible CFG
    // always reach the fixpoint path above. We still land here for cases
    // LoopSimplify cannot canonicalise — indirectbr-entered loops and
    // (genuinely irreducible) cycles that LoopInfo nevertheless recognises
    // as a natural loop with multiple entry edges.
    //
    // Strategy: a "materialize at every forward predecessor" option
    // is not directly implementable in our effects model. Each
    // Materialize effect emits one new invoke at its pred AND does a
    // function-wide RAUW (PartialEscapeTransform.cpp's applyMaterialize
    // Step 8). With N > 1 forward preds, the first apply RAUWs every
    // user of OrigAlloc to NewInv_0, but the post-N-th pred's path needs
    // NewInv_(N-1); without a synthesized PHI at the header to merge the
    // per-pred NewInvs, the in-loop uses would observe a single NewInv
    // that doesn't dominate them on all paths. The existing Case-A
    // multi-pred materialise pattern works only because there is already
    // an explicit PHI in IR whose incoming values MatPerBlock can patch.
    //
    // The sound pessimistic action for irreducible regions is to MARK
    // every VO still virtual at any
    // forward predecessor INELIGIBLE. commit() drops every Effect for an
    // ineligible VO and the original IR (alloc + stores + loads)
    // survives unchanged — the loop body's pre-loop pointer is the
    // original OrigAlloc on every entry edge, which trivially dominates
    // the body and is therefore SSA-correct.
    //
    // Loop-local allocs (created inside the body, consumed within a
    // single iteration) are still candidates: we walk the body once in
    // REGULAR mode below. They were never virtual at any forward pred
    // (the header's forward preds are outside L by definition), so the
    // ineligibility sweep above doesn't touch them. The fixpoint we
    // can't run is irrelevant for such allocs because they don't cross
    // the back-edge.
    //
    // Mark L in PessimisticLoops so the safety-net materializeBeforeLoops()
    // doesn't try to find a preheader to drain at.
    PessimisticLoops.insert(L);

    // Collect forward (non-loop-back) predecessors of the header.
    llvm::SmallVector<BasicBlock *, 4> ForwardPreds;
    llvm::SmallPtrSet<BasicBlock *, 4> Seen;
    for (BasicBlock *P : predecessors(Header)) {
      if (L->contains(P))
        continue;
      if (Seen.insert(P).second)
        ForwardPreds.push_back(P);
    }

    // Bail every VO that is virtual at any forward pred — drop it back
    // to the original IR. This is the irreducible-region bailout
    // semantics.
    // BlockExits[P] is populated by the outer RPO walk (forward preds
    // dominate the loop entry in RPO order, so they've been processed
    // by the time processLoop is invoked on L).
    for (BasicBlock *P : ForwardPreds) {
      auto It = BlockExits.find(P);
      if (It == BlockExits.end())
        continue;
      BlockExitInfo &PExit = It->second;
      for (jeandle::ObjectID ID : PExit.Virtuals) {
        Eligible[ID] = false;
      }
    }

    // Body walk in REGULAR mode (single pass — no fixpoint, since we have
    // no way to verify convergence at a non-existent preheader). Loop-
    // local allocs that don't outlive a single iteration (e.g. an alloc
    // allocated in the body and consumed before the back-edge) are
    // still virtualised — this preserves the earlier behaviour for
    // nested loops where the inner loop has no preheader (a common
    // shape under un-simplified IR).
    //
    // Soundness for cross-back-edge allocs: explicit IR PHIs at the
    // header with a back-edge virtual incoming go through Case A /
    // ineligible (no Case-B match because BlockExits[back-edge] hasn't
    // been populated when processBlock(header) runs); the original
    // alloc + stores survive. Loop-local allocs that NEVER cross a
    // back-edge are correctly folded (the body's single pass sees them
    // virtual at creation and they don't escape, so no Materialize
    // effect is emitted).
    llvm::SmallPtrSet<BasicBlock *, 16> _OuterDone;
    processLoopBodyOnePass(L, _OuterDone);
    return;
  }

  llvm::SmallPtrSet<BasicBlock *, 8> LoopBlocks;
  for (BasicBlock *BB : L->blocks())
    LoopBlocks.insert(BB);

  // At TOP-LEVEL processLoop entry only (loop.getDepth() == 1 gate),
  // compute the maximum loop depth within this nest. If it exceeds
  // JeandlePEALoopCutoff, transiently enter Mode::StopNewInLoopNest for
  // the duration of the fixpoint: tier1Allocate refuses NEW allocations,
  // but everything else (already-virtual tracking, merges, locks, loads,
  // stores, loop-exit handling) proceeds normally. The mode is restored
  // at the bottom of this function on convergence at depth==1.
  const Mode SavedModeForNest = CurrentMode;
  if (L->getLoopDepth() == 1) {
    // Count one outer-fixpoint entry per top-level processLoop
    // call. Nested processLoop calls are bookkept under the outer one.
    ++JeandlePEAOuterFixpointIterations;

    unsigned MaxDepth = L->getLoopDepth();
    for (Loop *Sub : L->getLoopsInPreorder())
      MaxDepth = std::max(MaxDepth, Sub->getLoopDepth());
    if (CurrentMode == Mode::Regular && MaxDepth > JeandlePEALoopCutoff)
      CurrentMode = Mode::StopNewInLoopNest;

    // Testing aid: optionally force MATERIALIZE_ALL for lit
    // coverage. Wins over StopNewInLoopNest because it more aggressively
    // exercises the deferred-materialise path.
    if (JeandlePEAForceMaterializeAll) {
      CurrentMode = Mode::MaterializeAll;
      // Count the forced escalation just like the non-test path.
      ++JeandlePEAModeEscalations;
    }

    // Reset the 2-stage escalation tracking on every top-level
    // processLoop entry. The flag is scoped to a single nest's overflow
    // recovery; new top-level loops start with a clean slate.
    TooManyIterationsSeen = false;
    OverflowFlag = false;
  } else {
    // Nested-loop entry. If an outer overflow is already in
    // progress (OverflowFlag latched by an inner recursion that tried to
    // touch an outer VO), unwind immediately — the outer-most processLoop
    // is the only sound rollback point.
    if (OverflowFlag)
      return;
  }

  // Snapshot pre-loop state.
  LoopSnapshot Pre;
  takeLoopSnapshot(L, LoopBlocks, Pre);

  // The fixpoint variable is "per-block exit info for every block in L".
  std::optional<DenseMap<BasicBlock *, BlockExitInfo>> LastExits;
  bool Converged = false;
  unsigned Iter = 0;
  for (; Iter < kMaxLoopFixpointIters; ++Iter) {
    if (Iter > 0)
      restoreLoopSnapshot(LoopBlocks, Pre);

    // Each iter is one inner-fixpoint pass. The total over a
    // run is the sum of body iterations across every processLoop call.
    ++JeandlePEALoopFixpointRetries;

    llvm::SmallPtrSet<BasicBlock *, 16> _OuterDone;
    processLoopBodyOnePass(L, _OuterDone);

    // A nested processLoop recursion's MATERIALIZE_ALL pass may
    // have latched OverflowFlag while touching an outer VO. Break the
    // fixpoint loop immediately: any further iters would walk a
    // half-consistent state and waste work. The outer non-convergence
    // path below handles the rollback + redo (only at top-level — at
    // depth>1 the recursion returned early via the OverflowFlag short-
    // circuit at the top of processLoop, so we never get here).
    if (OverflowFlag)
      break;

    // Capture this iteration's exits.
    DenseMap<BasicBlock *, BlockExitInfo> CurExits;
    for (BasicBlock *BB : LoopBlocks) {
      auto It = BlockExits.find(BB);
      if (It != BlockExits.end())
        CurExits[BB] = It->second;
    }
    // Refresh KnownAliveLoopEnds with every block that produced exit
    // state this iteration. Set semantics — once a block is "alive" we
    // expect it to remain alive in every subsequent iter; loopBlockExits-
    // Equivalent treats a missing entry as benign if the block was never
    // alive, but in debug builds we llvm_unreachable on a regression so a
    // monotonicity violation is caught instead of silently looping until
    // the iteration cap escalates to MATERIALIZE_ALL.
    for (auto &P : CurExits)
      KnownAliveLoopEnds[P.first] = true;
    if (LastExits &&
        loopBlockExitsEquivalent(LoopBlocks, *LastExits, CurExits)) {
      Converged = true;
      LLVM_DEBUG(dbgs() << "PEA: loop @ " << Header->getName()
                        << " converged in " << (Iter + 1) << " iters\n");
      break;
    }
    LastExits = std::move(CurExits);
  }
  // Scope KnownAliveLoopEnds to a single processLoop call. Different
  // sibling loops or nested loops re-enter processLoop with their own
  // monotonicity contract.
  KnownAliveLoopEnds.clear();

  if (Converged) {
    ConvergedLoops.insert(L);
    // Force-materialise at exits that flow into EH pads, so
    // exception handlers never see partially-materialised loop-internal
    // virtuals.
    processLoopExit(L);
    // Restore mode on convergence (currentMode reset to REGULAR at
    // depth==1 after success).
    if (L->getLoopDepth() == 1)
      CurrentMode = SavedModeForNest;
    return;
  }

  LLVM_DEBUG(dbgs() << "PEA: loop @ " << Header->getName()
                    << " failed to converge after "
                    << kMaxLoopFixpointIters
                    << " iters; falling back to MATERIALIZE_ALL\n");

  // Non-convergence: rollback and use the pessimistic preheader force-
  // materialize + MATERIALIZE_ALL body pass.
  restoreLoopSnapshot(LoopBlocks, Pre);
  // restoreLoopSnapshot intentionally preserves loop-block BlockExits
  // across iteration-to-iteration restores (so the next iter's
  // mergeStates sees the back-edge contribution). For the pessimistic
  // fallback, however, we want a TRUE pre-loop state: the upcoming
  // MaterializeAll body pass treats every loop-body alloc as fully
  // escaping, and inheriting iter-9's stale Virtual entries through
  // mergeStates(header) would re-introduce VOs the fallback is trying to
  // forget. Wipe loop-block BlockExits explicitly here. (BlockEffects
  // and MaterializedAtPred were already restored to pre-loop by
  // restoreLoopSnapshot, so we don't need to touch them.)
  for (BasicBlock *BB : LoopBlocks)
    BlockExits.erase(BB);

  // Drain preheader virtuals at PH terminator. Marks the loop so the
  // tail materializeBeforeLoops() sweep won't double-drain it.
  PessimisticLoops.insert(L);
  {
    auto It = BlockExits.find(Preheader);
    if (It != BlockExits.end()) {
      SmallVector<jeandle::ObjectID, 4> Vs(It->second.Virtuals.begin(),
                                            It->second.Virtuals.end());
      llvm::sort(Vs);
      for (jeandle::ObjectID ID : Vs) {
        if (!Eligible.lookup(ID))
          continue;
        materializeAtPredFromExitInfo(ID, Preheader, It->second);
      }
    }
  }

  Mode SavedMode = CurrentMode;
  // Count the Regular -> MaterializeAll escalation. We only bump
  // when the saved mode was NOT already MaterializeAll, so a forced
  // MATERIALIZE_ALL re-entry doesn't double-count. (The escalation path
  // may flip a second time on TooManyIterationsSeen; that is also
  // covered by the !was-MaterializeAll guard.)
  if (SavedMode != Mode::MaterializeAll)
    ++JeandlePEAModeEscalations;
  CurrentMode = Mode::MaterializeAll;
  // Also drain preheader virtuals via the canonical helper before
  // the MATERIALIZE_ALL body pass — this is identical in effect to the
  // hand-rolled drain just above, but using the helper makes the path
  // shared with the retry pass below. (Both calls are idempotent because
  // materializeAtPredFromExitInfo checks MaterializedAtPred and Eligible.)
  processStateBeforeLoopOnOverflow(L);
  {
    llvm::SmallPtrSet<BasicBlock *, 16> _OuterDone;
    processLoopBodyOnePass(L, _OuterDone);
  }

  // 2-stage escalation. After the MATERIALIZE_ALL body pass, run a
  // convergence re-check by comparing the post-pass BlockExits of every
  // loop block against the iter-9 (last regular) snapshot. If non-
  // equivalent AND TooManyIterationsSeen is still false, set the flag and
  // run one more MATERIALIZE_ALL pass. If it's still non-equivalent on
  // the second observation, hard-bail (unreachable in debug).
  auto captureCurExits = [&]() {
    DenseMap<BasicBlock *, BlockExitInfo> M;
    for (BasicBlock *BB : LoopBlocks) {
      auto It = BlockExits.find(BB);
      if (It != BlockExits.end())
        M[BB] = It->second;
    }
    return M;
  };
  if (L->getLoopDepth() == 1 && LastExits) {
    DenseMap<BasicBlock *, BlockExitInfo> MAExits = captureCurExits();
    if (!loopBlockExitsEquivalent(LoopBlocks, *LastExits, MAExits)) {
      if (!TooManyIterationsSeen) {
        TooManyIterationsSeen = true;
        // Restore from pre-loop snapshot, then re-drain + re-run the
        // body pass — the second-stage retry.
        restoreLoopSnapshot(LoopBlocks, Pre);
        for (BasicBlock *BB : LoopBlocks)
          BlockExits.erase(BB);
        processStateBeforeLoopOnOverflow(L);
        llvm::SmallPtrSet<BasicBlock *, 16> _OuterDone2;
        processLoopBodyOnePass(L, _OuterDone2);
#ifndef NDEBUG
        // In debug, assert that the second pass converged: a true
        // MATERIALIZE_ALL body pass should produce no new VOs, and the
        // overflow-recovery drain should leave no live virtuals on
        // entry. If that's not the case there's an analyzer invariant
        // bug we want to surface — but only in debug; release continues
        // silently because the IR is still sound (everything still
        // virtual at this point survives in IR with original alloc).
        DenseMap<BasicBlock *, BlockExitInfo> MAExits2 = captureCurExits();
        if (!loopBlockExitsEquivalent(LoopBlocks, MAExits, MAExits2)) {
          // Conservative bail rather than llvm_unreachable: in practice
          // this branch indicates a pathological IR that exposes a
          // missing per-VO invariant somewhere. Marking every still-
          // virtual VO ineligible and proceeding restores the original
          // IR (all stores survive) — sound but pessimistic.
          for (BasicBlock *BB : LoopBlocks) {
            auto EIt = BlockExits.find(BB);
            if (EIt == BlockExits.end())
              continue;
            for (jeandle::ObjectID ID : EIt->second.Virtuals)
              Eligible[ID] = false;
          }
        }
#endif
      }
    }
  }

  CurrentMode = SavedMode;
  // Also restore the nest-entry SavedMode on the non-convergence
  // exit path. Today SavedMode == SavedModeForNest at depth==1 because no
  // re-entry happens between them, but be explicit so future changes don't
  // accidentally leak StopNewInLoopNest past the top-level processLoop.
  if (L->getLoopDepth() == 1)
    CurrentMode = SavedModeForNest;
}

jeandle::PEAResult Analyzer::run() {
  // Filter PEA to functions matching a name substring. Empty
  // option (the default) lets every gated Java method through unchanged.
  if (!JeandleEscapeAnalyzeOnly.empty() &&
      !F.getName().contains(JeandleEscapeAnalyzeOnly))
    return jeandle::PEAResult();

  // Pre-compute the set of loop headers for the LoopFieldPhiCache gate.
  for (BasicBlock &BB : F)
    if (LI.isLoopHeader(&BB))
      LoopHeaderSet.insert(&BB);

  // Outer walk: RPO over F. When we hit any block belonging to a top-level
  // loop, dispatch to processLoop on that top-level loop (which recursively
  // handles sub-loops). All other blocks are processed directly.
  llvm::SmallPtrSet<BasicBlock *, 16> Done;
  ReversePostOrderTraversal<Function *> RPOT(&F);
  // Defensive sweep for cycles LoopInfo missed (indirectbr /
  // callbr / catchswitch back-edges). When the RPO walk reaches a non-
  // loop block whose preds include an as-yet-UNVISITED block, that
  // unvisited predecessor is a back-edge that LoopInfo did not
  // recognise. Without intervention, mergeStates would silently drop
  // the back-edge contribution (no exit data for it yet); the resulting
  // virtual state at BB would not reflect the cross-iteration field
  // mutations. Bail every still-virtual VO at BB entry to ineligibility
  // so the original IR survives unchanged.
  auto bailUnvisitedBackEdgeVOs = [&](BasicBlock *BB) {
    if (BB == &F.getEntryBlock())
      return;
    bool HasUnvisitedPred = false;
    for (BasicBlock *P : predecessors(BB)) {
      if (!Done.count(P)) {
        HasUnvisitedPred = true;
        break;
      }
    }
    if (!HasUnvisitedPred)
      return;
    // Conservative ineligibility flip on every VO that is currently
    // tracked as virtual in ANY processed pred's exit. Mirrors the
    // forward-pred sweep in the no-preheader fallback.
    for (BasicBlock *P : predecessors(BB)) {
      auto It = BlockExits.find(P);
      if (It == BlockExits.end())
        continue;
      for (jeandle::ObjectID ID : It->second.Virtuals)
        Eligible[ID] = false;
    }
    BlockExits.erase(BB);
  };
  for (BasicBlock *BB : RPOT) {
    if (Done.count(BB))
      continue;
    Loop *L = LI.getLoopFor(BB);
    if (!L) {
      bailUnvisitedBackEdgeVOs(BB);
      processBlock(BB);
      Done.insert(BB);
      continue;
    }
    // Find the top-level loop containing BB. Sub-loops are dispatched
    // recursively from inside processLoop on the top-level loop.
    Loop *Top = L;
    while (Top->getParentLoop())
      Top = Top->getParentLoop();
    // If we haven't reached the top-level header in RPO yet (e.g. when a
    // block inside the loop appears before its header), still dispatch on
    // the top-level loop and mark all its blocks Done.
    processLoop(Top);
    for (BasicBlock *SB : Top->blocks())
      Done.insert(SB);
  }

  // Safety net — drain preheader virtuals ONLY for loops the RPO
  // walk never reached (unreachable top-level loops, or sub-loops whose
  // outer recursion bailed before recursing). Everything else has been
  // handled by processLoop directly. Loops with no preheader are a no-op
  // either way (the function cannot pick a drain IP without a PH).
  materializePreheaderVirtualsForUnvisitedLoops();
  commit();
  return std::move(Result);
}

} // anonymous namespace

PartialEscapeAnalysis::Result
PartialEscapeAnalysis::run(Function &F, FunctionAnalysisManager &FAM) {
  // Gate on jeandle.java_method_compilation: only Java methods are analyzed
  // (template module / runtime stubs are skipped).
  Module *M = F.getParent();
  if (!M || !M->getNamedMetadata(jeandle::Metadata::JavaMethodCompilation))
    return jeandle::PEAResult();

  // Request DominatorTree and LoopInfo eagerly so they're cached for later
  // PEA passes that need them.
  auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
  auto &LI = FAM.getResult<LoopAnalysis>(F);

  Analyzer A(F, DT, LI);
  return A.run();
}
