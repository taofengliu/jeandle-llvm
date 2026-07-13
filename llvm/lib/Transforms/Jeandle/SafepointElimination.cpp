//===- SafepointElimination.cpp - Jeandle Safepoint Elimination -----------===//
//
// Copyright (c) 2026, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// Removes redundant `jeandle.safepoint_poll` calls, so that loop
/// optimizations are not blocked by opaque calls and the per-iteration poll
/// overhead is reduced, while every loop stays within the configured
/// time-to-safepoint budget: a poll is only deleted when another poll keeps
/// covering the loop or the loop's trip count is provably small.
///
/// Current transforms:
///   - Adjacent poll collapse: two polls back to back on a straight-line path
///     collapse to the later one, whose deopt state supersedes the earlier
///     one's at that program point. Mirrors C2's SafePointNode::Identity and
///     Graal's SafepointNode.simplify.
///   - Keep-one dedup: when some poll dominates a loop's latch, every
///     iteration passes it, so the loop's other polls are redundant. Mirrors
///     C2's IdealLoopTree::remove_safepoints(keep_one=true).
///   - Short-loop deletion: an innermost loop whose trip count provably fits
///     the chunk budget cannot keep a thread away from a safepoint for more
///     than that many iterations, so its polls go entirely. Mirrors C2's
///     short-loop collapse in OuterStripMinedLoopNode::adjust_strip_mined_loop.
///   - Strip mining (off by default, -jeandle-enable-strip-mining): an
///     innermost counted loop whose trip count can't be bounded is wrapped in
///     an outer loop that runs the inner loop in batches of chunk-iters; the
///     back-edge poll is relocated to the outer back-edge so the inner loop is
///     poll-free while time-to-safepoint stays bounded. Mirrors C2's
///     OuterStripMinedLoopNode.
///   - Inclusive loop versioning: a signed inclusive loop whose runtime limit
///     might let its IV wrap is cloned behind a no-wrap guard. The safe version
///     can be strip-mined; the extreme version keeps its original poll.
///   - Empty-loop deletion: remove a finite, side-effect-free loop atomically
///     with the poll that prevents its deletion.
///
/// A poll tagged `!poll-coverage` is one some transform designated as
/// its loop's required coverage; it wins over the positional rule. Strip
/// mining additionally tags the inner latch `!strip-mined` to record
/// that the inner loop is bounded by construction (the coverage verifier reads
/// this where SCEV can't recover the bound).
///
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Jeandle/SafepointElimination.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Jeandle/Attributes.h"
#include "llvm/IR/Jeandle/Metadata.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ValueMap.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"
#include <optional>

using namespace llvm;

#define DEBUG_TYPE "safepoint-elimination"

static cl::opt<bool> EnableSafepointElim(
    "jeandle-enable-safepoint-elim", cl::init(true),
    cl::desc("Master switch for the SafepointElimination pass. Setting this "
             "to false makes the pass a no-op. Useful for A/B comparison."));

// One knob, three meanings: the bare-deletion trip bound, the strip-mining
// chunk size, and therefore the system-wide bound on how many iterations a
// thread can run between safepoint polls. In the JVM the frontend sets this
// from -XX:JeandleStripMiningIter, so raising that flag to coarsen strip-mining
// batches also raises the short-loop deletion threshold — the two are the same
// budget by design. C2's equivalent is LoopStripMiningIter (default 1000).
static cl::opt<uint64_t> SafepointChunkIters(
    "jeandle-safepoint-chunk-iters", cl::init(1000),
    cl::desc("Iteration budget between safepoint polls: also the strip-mining "
             "batch size. Polls are deleted outright only in innermost loops "
             "whose SCEV max backedge-taken count provably fits this bound."));

uint64_t llvm::jeandle::getSafepointChunkIters() { return SafepointChunkIters; }

// Strip mining. When a counted loop's trip count is not provably within the
// chunk budget, its back-edge poll can't be deleted outright. Wrap the loop in
// an outer loop that runs the inner loop in batches of SafepointChunkIters,
// leaving the inner loop poll-free and carrying a single poll on the outer
// back-edge. This LLVM knob defaults off (so opt/lit and non-Jeandle callers
// are unaffected); in the JVM the frontend turns it on and sets the chunk size
// from -XX:JeandleStripMining / -XX:JeandleStripMiningIter (default on),
// meaning production Jeandle compiles run with strip mining enabled.
static cl::opt<bool> EnableStripMining(
    "jeandle-enable-strip-mining", cl::init(false),
    cl::desc("Strip-mine unbounded/large counted loops, bounding GC latency to "
             "-jeandle-safepoint-chunk-iters inner iterations between polls."));

bool llvm::jeandle::isStripMiningEnabled() { return EnableStripMining; }

static cl::opt<bool> EnableInclusiveLoopVersioning(
    "jeandle-enable-inclusive-loop-versioning", cl::init(false),
    cl::desc("Clone supported runtime-risk inclusive loops behind a no-wrap "
             "guard so the safe version can be strip-mined."));

bool llvm::jeandle::isInclusiveLoopVersioningEnabled() {
  return EnableInclusiveLoopVersioning;
}

bool llvm::jeandle::isSafepointPoll(const Instruction &I) {
  const auto *CI = dyn_cast<CallInst>(&I);
  if (!CI || CI->isIndirectCall())
    return false;
  const Function *Callee = CI->getCalledFunction();
  return Callee && Callee->getName() == "jeandle.safepoint_poll";
}

namespace {

using jeandle::isSafepointPoll;

// A poll carrying this metadata is the designated safepoint coverage of its
// loop: the loop must retain at least one poll so tagged. The tag marks the
// loop's coverage need, not the instruction's identity — clones inherit it
// and any one of them may serve. Name lives in Metadata.h.
constexpr StringRef PollCoverageMD = jeandle::Metadata::PollCoverage;
constexpr StringRef InclusiveSlowPathMD = "jeandle.inclusive.slow";

bool hasCoverageMarker(const Instruction &I) {
  return I.getMetadata(PollCoverageMD) != nullptr;
}

static bool hasConstantMaxBackedgeWithinBudget(Loop &L, ScalarEvolution &SE) {
  const auto *MaxC =
      dyn_cast<SCEVConstant>(SE.getConstantMaxBackedgeTakenCount(&L));
  return MaxC && MaxC->getAPInt().ule(SafepointChunkIters);
}

static bool hasDirectDominatingPoll(Loop &L, LoopInfo &LI, DominatorTree &DT) {
  BasicBlock *Latch = L.getLoopLatch();
  if (!Latch)
    return false;
  for (BasicBlock *BB : L.blocks()) {
    if (LI.getLoopFor(BB) != &L || !DT.dominates(BB, Latch))
      continue;
    for (Instruction &I : *BB)
      if (isSafepointPoll(I))
        return true;
  }
  return false;
}

static bool isRequiredByAncestorLoop(CallInst *P, Loop &L, LoopInfo &LI,
                                     DominatorTree &DT, ScalarEvolution &SE) {
  BasicBlock *PollBB = P->getParent();
  for (Loop *Ancestor = L.getParentLoop(); Ancestor;
       Ancestor = Ancestor->getParentLoop()) {
    if (hasConstantMaxBackedgeWithinBudget(*Ancestor, SE))
      continue;
    BasicBlock *AncestorLatch = Ancestor->getLoopLatch();
    if (!AncestorLatch)
      return true;
    if (!DT.dominates(PollBB, AncestorLatch))
      continue;
    if (hasDirectDominatingPoll(*Ancestor, LI, DT))
      continue;
    return true;
  }
  return false;
}

// Debug/pseudo-probe and lifetime markers carry no observable side effect, so
// they don't break the back-to-back adjacency of two polls.
bool isAdjacencyTransparent(const Instruction &I) {
  return I.isDebugOrPseudoInst() || I.isLifetimeStartOrEnd();
}

// Collapse runs of adjacent polls within a block, keeping one. Two polls with
// nothing observable between them are redundant; each carries its own
// self-contained deopt state, so dropping either is safe (no state is merged
// or moved). We keep the later by convention (as C2's SafePointNode::Identity
// does), and the coverage-marked one if either is marked, regardless of
// position.
bool collapseAdjacentPolls(BasicBlock &BB) {
  bool Changed = false;
  CallInst *Prev = nullptr;
  for (Instruction &I : llvm::make_early_inc_range(BB)) {
    if (isSafepointPoll(I)) {
      auto *Cur = cast<CallInst>(&I);
      if (Prev) {
        CallInst *Victim =
            (hasCoverageMarker(*Prev) && !hasCoverageMarker(*Cur)) ? Cur : Prev;
        CallInst *Survivor = Victim == Prev ? Cur : Prev;
        LLVM_DEBUG(dbgs() << "  collapse: erased an adjacent poll in "
                          << BB.getName() << "\n");
        Victim->eraseFromParent();
        Changed = true;
        Prev = Survivor;
        continue;
      }
      Prev = Cur;
      continue;
    }
    if (!isAdjacencyTransparent(I))
      Prev = nullptr;
  }
  return Changed;
}

// Keep-one dedup, C2's remove_safepoints(keep_one=true). When some poll the
// loop owns dominates the latch, every complete iteration passes it, so the
// loop's other polls are redundant: keep the latch-closest dominating poll (a
// coverage-marked one wins) and erase the rest. Without a dominating poll,
// deleting any poll could leave an iteration path uncovered — delete nothing.
//
// Only polls in blocks the loop owns directly are considered; a sub-loop's
// poll is not taken as this loop's coverage even if it dominates the latch.
// This is deliberate conservatism: relying on a poll this loop doesn't own
// would leave the loop naked if a later transform (full unroll, LoopDeletion,
// vectorization, the second-pass run) removes that sub-loop poll. Keeping each
// loop self-covered also keeps the coverage invariant locally verifiable.
// (Loops are processed innermost-first, so a sub-loop's polls are already
// finalized here — the concern is downstream passes, not this one.)
bool keepOneLoopPoll(Loop &L, LoopInfo &LI, DominatorTree &DT,
                     ScalarEvolution &SE) {
  BasicBlock *Latch = L.getLoopLatch();
  if (!Latch)
    return false;

  SmallVector<CallInst *, 4> Polls;
  for (BasicBlock *BB : L.blocks()) {
    if (LI.getLoopFor(BB) != &L)
      continue;
    for (Instruction &I : *BB)
      if (isSafepointPoll(I))
        Polls.push_back(cast<CallInst>(&I));
  }
  if (Polls.size() < 2)
    return false;

  SmallVector<CallInst *, 4> AncestorRequiredPolls;
  for (CallInst *P : Polls)
    if (isRequiredByAncestorLoop(P, L, LI, DT, SE))
      AncestorRequiredPolls.push_back(P);
  auto IsAncestorRequired = [&](CallInst *P) {
    return llvm::is_contained(AncestorRequiredPolls, P);
  };

  // Walk the dominator chain upward from the latch; every block on it
  // dominates the latch, so the first poll found is the latch-closest
  // dominating one. Ancestor-required polls win first: a poll needed by an
  // enclosing unbounded loop must not be deleted merely because a later inner
  // poll is a better local coverage point.
  CallInst *Keep = nullptr;
  for (bool RequiredOnly : {true, false}) {
    if (RequiredOnly && AncestorRequiredPolls.empty())
      continue;
    for (bool MarkedOnly : {true, false}) {
      for (BasicBlock *BB = Latch; BB && L.contains(BB);) {
        if (LI.getLoopFor(BB) == &L) {
          for (Instruction &I : llvm::reverse(*BB)) {
            if (isSafepointPoll(I) && (!MarkedOnly || hasCoverageMarker(I))) {
              auto *P = cast<CallInst>(&I);
              if (!RequiredOnly || IsAncestorRequired(P)) {
                Keep = P;
                break;
              }
            }
          }
          if (Keep)
            break;
        }
        auto *IDom = DT.getNode(BB)->getIDom();
        BB = IDom ? IDom->getBlock() : nullptr;
      }
      if (Keep)
        break;
    }
    if (Keep)
      break;
  }
  if (!Keep)
    return false;

  LLVM_DEBUG(dbgs() << "  keep-one: loop " << L.getHeader()->getName()
                    << " kept the latch-dominating poll, erased "
                    << (Polls.size() - 1) << " redundant\n");
  for (CallInst *P : Polls)
    if (P != Keep && !IsAncestorRequired(P))
      P->eraseFromParent();

  // The survivor is the loop's designated coverage from here on.
  if (!hasCoverageMarker(*Keep))
    Keep->setMetadata(PollCoverageMD, MDNode::get(Keep->getContext(), {}));
  return true;
}

// Trip-count-based deletion. SCEV's constant max backedge-taken count is a
// sound upper bound across every exit and wrap case, so when it fits the
// chunk budget the loop cannot keep a thread away from its next safepoint
// for more than the budget and its polls can go entirely.
//
// Innermost loops only: a short loop enclosing a poll-free short loop would
// compound to budget^2 poll-free iterations and break the bound. Once the
// inner loop dissolves (full unroll, deletion), the enclosing loop becomes
// innermost and qualifies on a later run.
//
// This deletes the loop's own coverage-marked polls too: a provable trip bound
// supersedes the current loop's coverage need. Polls that are also required
// coverage for an unbounded ancestor loop are not deleted.
bool deleteShortLoopPolls(Loop &L, LoopInfo &LI, DominatorTree &DT,
                          ScalarEvolution &SE) {
  if (!L.isInnermost())
    return false;
  if (!hasConstantMaxBackedgeWithinBudget(L, SE))
    return false;

  SmallVector<CallInst *, 4> Polls;
  SmallVector<CallInst *, 4> DeletablePolls;
  for (BasicBlock *BB : L.blocks())
    for (Instruction &I : *BB)
      if (isSafepointPoll(I))
        Polls.push_back(cast<CallInst>(&I));
  if (Polls.empty())
    return false;

  for (CallInst *P : Polls)
    if (!isRequiredByAncestorLoop(P, L, LI, DT, SE))
      DeletablePolls.push_back(P);
  if (DeletablePolls.empty())
    return false;

  LLVM_DEBUG(dbgs() << "  short-loop: " << L.getHeader()->getName()
                    << " max trip count fits the chunk budget, deleted "
                    << DeletablePolls.size() << " poll(s), kept "
                    << (Polls.size() - DeletablePolls.size())
                    << " ancestor-required\n");
  for (CallInst *P : DeletablePolls)
    P->eraseFromParent();
  SE.forgetLoop(&L);
  return true;
}

struct EmptyLoopExitValueCheck {
  PHINode *Phi;
  Value *OriginalValue;
  const SCEV *ExitValue;
};

struct EmptyLoopPollRemovalPlan {
  Loop *L;
  BasicBlock *Preheader;
  BasicBlock *ExitingBlock;
  BasicBlock *ExitBlock;
  SmallVector<CallInst *, 2> Polls;
  SmallVector<EmptyLoopExitValueCheck, 2> ExitValues;
};

bool hasOnlyDeoptStateUses(Value *V, Loop &L, SmallPtrSetImpl<Value *> &Visited,
                           bool &ReachesPoll) {
  if (!Visited.insert(V).second)
    return true;
  for (User *U : V->users()) {
    auto *I = dyn_cast<Instruction>(U);
    if (!I)
      return false;
    if (isSafepointPoll(*I)) {
      ReachesPoll = true;
      continue;
    }
    if (I->isDebugOrPseudoInst())
      continue;
    auto *Phi = dyn_cast<PHINode>(I);
    if (!Phi || L.contains(Phi) ||
        !hasOnlyDeoptStateUses(Phi, L, Visited, ReachesPoll))
      return false;
  }
  return true;
}

// A loop is empty for this transform only when it contains pure control/IV
// computation plus direct Jeandle polls. Memory accesses and other calls stay
// out of scope even when LLVM could otherwise prove them dead.
std::optional<EmptyLoopPollRemovalPlan>
buildEmptyLoopPollRemovalPlan(Loop *L, LoopInfo &LI, DominatorTree &DT,
                              ScalarEvolution &SE) {
  if (!L->isInnermost() || !L->isLCSSAForm(DT) || !L->hasDedicatedExits())
    return std::nullopt;

  BasicBlock *Preheader = L->getLoopPreheader();
  BasicBlock *ExitingBlock = L->getExitingBlock();
  BasicBlock *ExitBlock = L->getUniqueExitBlock();
  if (!Preheader || !ExitingBlock || !ExitBlock || ExitBlock->isEHPad() ||
      ExitBlock->getSinglePredecessor() != ExitingBlock)
    return std::nullopt;

  auto *PreheaderBr = dyn_cast<BranchInst>(Preheader->getTerminator());
  if (!PreheaderBr || !PreheaderBr->isUnconditional() ||
      PreheaderBr->getSuccessor(0) != L->getHeader())
    return std::nullopt;

  // Deleting an otherwise empty loop is valid only when it is known to
  // terminate.
  if (isa<SCEVCouldNotCompute>(SE.getConstantMaxBackedgeTakenCount(L)))
    return std::nullopt;

  SmallVector<CallInst *, 2> Polls;
  for (BasicBlock *BB : L->blocks()) {
    for (Instruction &I : *BB) {
      if (isSafepointPoll(I)) {
        Polls.push_back(cast<CallInst>(&I));
        continue;
      }
      if (isa<CallBase>(I) || I.mayReadOrWriteMemory() ||
          I.mayHaveSideEffects())
        return std::nullopt;
    }
  }
  if (Polls.empty())
    return std::nullopt;
  for (CallInst *P : Polls)
    if (isRequiredByAncestorLoop(P, *L, LI, DT, SE))
      return std::nullopt;

  SCEVExpander Rewriter(SE, "jeandle-empty-loop-check");
  SmallVector<EmptyLoopExitValueCheck, 2> ExitValues;
  bool HasDeoptLiveOut = false;
  // Keep this narrow: reconstruct values that survive solely for safepoint
  // deopt state, not general reductions with observable live-outs.
  for (PHINode &Phi : ExitBlock->phis()) {
    if (Phi.getNumIncomingValues() != 1 ||
        Phi.getIncomingBlock(0) != ExitingBlock)
      return std::nullopt;

    Value *Incoming = Phi.getIncomingValue(0);
    auto *IncomingInst = dyn_cast<Instruction>(Incoming);
    if (!IncomingInst || !L->contains(IncomingInst))
      continue;

    SmallPtrSet<Value *, 8> Visited;
    bool ReachesPoll = false;
    if (!hasOnlyDeoptStateUses(&Phi, *L, Visited, ReachesPoll) || !ReachesPoll)
      return std::nullopt;
    HasDeoptLiveOut = true;
    if (!SE.isSCEVable(Phi.getType()))
      return std::nullopt;

    const SCEV *ExitValue = SE.getSCEVAtScope(IncomingInst, L->getParentLoop());
    if (isa<SCEVCouldNotCompute>(ExitValue) ||
        !SE.isLoopInvariant(ExitValue, L) ||
        !Rewriter.isSafeToExpandAt(ExitValue, Preheader->getTerminator()))
      return std::nullopt;
    ExitValues.push_back({&Phi, Incoming, ExitValue});
  }
  if (!HasDeoptLiveOut)
    return std::nullopt;

  return EmptyLoopPollRemovalPlan{
      L,         Preheader,        ExitingBlock,
      ExitBlock, std::move(Polls), std::move(ExitValues)};
}

bool stillStructurallyValid(const EmptyLoopPollRemovalPlan &Plan, LoopInfo &LI,
                            DominatorTree &DT) {
  Loop *L = Plan.L;
  if (!L->isInnermost() || LI.getLoopFor(L->getHeader()) != L ||
      L->getLoopPreheader() != Plan.Preheader ||
      L->getExitingBlock() != Plan.ExitingBlock ||
      L->getUniqueExitBlock() != Plan.ExitBlock || !L->isLCSSAForm(DT) ||
      !L->hasDedicatedExits() ||
      Plan.ExitBlock->getSinglePredecessor() != Plan.ExitingBlock)
    return false;
  for (CallInst *P : Plan.Polls)
    if (!P->getParent() || !L->contains(P) || !isSafepointPoll(*P))
      return false;
  for (const EmptyLoopExitValueCheck &ExitValue : Plan.ExitValues)
    if (ExitValue.Phi->getNumIncomingValues() != 1 ||
        ExitValue.Phi->getIncomingBlock(0) != Plan.ExitingBlock ||
        ExitValue.Phi->getIncomingValue(0) != ExitValue.OriginalValue)
      return false;
  return true;
}

void applyEmptyLoopPollRemovalPlan(const EmptyLoopPollRemovalPlan &Plan,
                                   LoopInfo &LI, DominatorTree &DT,
                                   ScalarEvolution &SE) {
  SCEVExpander Rewriter(SE, "jeandle-empty-loop-delete");
  for (const EmptyLoopExitValueCheck &ExitValue : Plan.ExitValues) {
    Value *Replacement =
        Rewriter.expandCodeFor(ExitValue.ExitValue, ExitValue.Phi->getType(),
                               Plan.Preheader->getTerminator());
    ExitValue.Phi->setIncomingValue(0, Replacement);
  }

  LLVM_DEBUG(dbgs() << "  loop-deletion-prep: deleting "
                    << Plan.L->getHeader()->getName() << " and "
                    << Plan.Polls.size() << " poll(s)\n");
  for (CallInst *P : Plan.Polls)
    P->eraseFromParent();
  deleteDeadLoop(Plan.L, &DT, &SE, &LI);
}

// ===--------------------------------------------------------------------===//
// Strip mining for unbounded / large counted loops.
//
// A loop whose trip count is not provably within the chunk budget can't have
// its back-edge poll deleted without leaving the loop naked. Following C2's
// OuterStripMinedLoopNode, wrap the loop in an outer loop that iterates the
// original IV space in batches of N: the inner loop runs poll-free (so it can
// vectorize / LICM / widen its IV) and a single poll on the outer back-edge
// keeps time-to-safepoint bounded to N inner iterations.
//
// The poll is relocated, never synthesized: the inner back-edge poll is
// cloned onto the outer latch with its deopt bundle carried over and the
// loop-carried operands remapped to the outer recurrences. A poll whose deopt
// state can't be remapped — it references a value that is neither the IV, a
// header recurrence, nor loop-invariant — makes the loop ineligible; we keep
// it as-is rather than emit a safepoint with wrong deopt state.
// ===--------------------------------------------------------------------===//

struct IVInfo {
  PHINode *Phi;
  Value *ComparedValue;
  APInt Step;
  const SCEVAddRecExpr *AR;
};

std::optional<APInt> getConstantAddStep(Value *V, PHINode *Phi) {
  auto *BO = dyn_cast<BinaryOperator>(V);
  if (!BO || BO->getOpcode() != Instruction::Add)
    return std::nullopt;

  Value *Other = nullptr;
  if (BO->getOperand(0) == Phi)
    Other = BO->getOperand(1);
  else if (BO->getOperand(1) == Phi)
    Other = BO->getOperand(0);
  else
    return std::nullopt;

  auto *Step = dyn_cast<ConstantInt>(Other);
  if (!Step || Step->isZero())
    return std::nullopt;
  return Step->getValue();
}

// The integer induction phi that ExitCmp compares directly against a
// loop-invariant limit, with a compile-time non-zero constant step. Header
// exits compare the phi itself; canonical latch exits compare the latch-carried
// next value. In both cases the cmp's limit rewrite stays a single operand swap
// and the IV is tied to the exit test, so a loop with several affine
// recurrences strip-mines on the one the exit drives.
std::optional<IVInfo> findIntInduction(Loop *L, ICmpInst *ExitCmp,
                                       ScalarEvolution &SE) {
  Value *Op0 = ExitCmp->getOperand(0);
  Value *Op1 = ExitCmp->getOperand(1);
  BasicBlock *Latch = L->getLoopLatch();
  for (PHINode &Phi : L->getHeader()->phis()) {
    Value *LatchValue = Latch ? Phi.getIncomingValueForBlock(Latch) : nullptr;
    Value *Limit;
    Value *ComparedValue;
    if (Op0 == &Phi || Op0 == LatchValue) {
      Limit = Op1;
      ComparedValue = Op0;
    } else if (Op1 == &Phi || Op1 == LatchValue) {
      Limit = Op0;
      ComparedValue = Op1;
    } else {
      continue;
    }
    if (!Phi.getType()->isIntegerTy() || !SE.isSCEVable(Phi.getType()))
      continue;
    const auto *AR = dyn_cast<SCEVAddRecExpr>(SE.getSCEV(&Phi));
    if (!AR || AR->getLoop() != L || !AR->isAffine())
      continue;
    const auto *Step = dyn_cast<SCEVConstant>(AR->getStepRecurrence(SE));
    if (!Step || Step->getAPInt().isZero())
      continue;
    if (!SE.isLoopInvariant(SE.getSCEV(Limit), L))
      continue;
    return IVInfo{&Phi, ComparedValue, Step->getAPInt(), AR};
  }
  return std::nullopt;
}

SmallVector<ICmpInst *, 2> collectExitConditions(Loop *L) {
  SmallVector<BasicBlock *, 4> Exits;
  L->getExitingBlocks(Exits);
  SmallVector<ICmpInst *, 2> Out;
  for (BasicBlock *BB : Exits)
    if (auto *Br = dyn_cast<BranchInst>(BB->getTerminator()))
      if (Br->isConditional())
        if (auto *Cmp = dyn_cast<ICmpInst>(Br->getCondition()))
          Out.push_back(Cmp);
  return Out;
}

// Relational predicates are handled directly. eq/ne loops are accepted here so
// checkStripMineShape can support the `i != limit` counted-loop subset when the
// continue predicate normalizes to NE and the step is exactly +/-1.
bool isAcceptablePredicate(ICmpInst::Predicate P) {
  return ICmpInst::isRelational(P) || P == ICmpInst::ICMP_EQ ||
         P == ICmpInst::ICMP_NE;
}

// KEEP IN SYNC WITH SafepointCoverageVerifier.cpp's no-wrap helpers. The
// transform proves this on the original loop; the verifier re-proves the same
// latch-add obligation on the strip-mined IR. For an exclusive test, the latch
// add runs only for values that pass `iv < limit` / `iv > limit`: increasing
// IVs need `limit <= Max - step + 1`; decreasing IVs need
// `limit >= Min + abs(step) - 1`.
bool canProveExclusiveNoWrap(const IVInfo &IV, const SCEV *LimitS,
                             const Instruction *CtxI, bool IsSigned,
                             bool Increasing, ScalarEvolution &SE) {
  if (IV.Step.abs().isOne())
    return true;
  if (IsSigned ? IV.AR->hasNoSignedWrap() : IV.AR->hasNoUnsignedWrap())
    return true;

  unsigned BW = IV.Phi->getType()->getIntegerBitWidth();
  ICmpInst::Predicate Pred;
  APInt Bound;
  if (Increasing) {
    Pred = IsSigned ? ICmpInst::ICMP_SLE : ICmpInst::ICMP_ULE;
    Bound = IsSigned ? APInt::getSignedMaxValue(BW) : APInt::getMaxValue(BW);
    Bound -= IV.Step;
    Bound += 1;
  } else {
    if (IV.Step.isMinSignedValue())
      return false;
    APInt AbsStep = IV.Step.abs();
    Pred = IsSigned ? ICmpInst::ICMP_SGE : ICmpInst::ICMP_UGE;
    Bound = IsSigned ? APInt::getSignedMinValue(BW) : APInt::getMinValue(BW);
    Bound += AbsStep;
    Bound -= 1;
  }

  return SE.isKnownPredicateAt(Pred, LimitS, SE.getConstant(Bound), CtxI);
}

bool isStableAcrossUsesImpl(const Value *V,
                            SmallPtrSetImpl<const Value *> &Visiting) {
  if (isa<FreezeInst>(V))
    return true;
  if (!isGuaranteedNotToBeUndefOrPoison(V))
    return false;
  if (isa<Constant>(V) || isa<Argument>(V))
    return true;

  auto *Op = dyn_cast<Operator>(V);
  if (!Op || isa<CallBase>(V) || isa<LoadInst>(V) || !Visiting.insert(V).second)
    return false;
  bool Stable = llvm::all_of(Op->operands(), [&](const Use &U) {
    return isStableAcrossUsesImpl(U.get(), Visiting);
  });
  Visiting.erase(V);
  return Stable;
}

bool isStableAcrossUses(const Value *V) {
  SmallPtrSet<const Value *, 8> Visiting;
  return isStableAcrossUsesImpl(V, Visiting);
}

// KEEP IN SYNC WITH SafepointCoverageVerifier.cpp's no-wrap helpers. Inclusive
// tests execute the boundary value itself, so the real limit must stay one step
// farther from the type edge than the exclusive case unless SCEV already proves
// the addrec no-wrap.
bool canProveInclusiveNoWrap(const IVInfo &IV, Value *Limit, const SCEV *LimitS,
                             const Instruction *CtxI, bool IsSigned,
                             bool Increasing, ScalarEvolution &SE) {
  if (!isStableAcrossUses(Limit))
    return false;
  if (IsSigned ? IV.AR->hasNoSignedWrap() : IV.AR->hasNoUnsignedWrap())
    return true;

  unsigned BW = IV.Phi->getType()->getIntegerBitWidth();
  ICmpInst::Predicate Pred;
  APInt Margin;
  if (Increasing) {
    Pred = IsSigned ? ICmpInst::ICMP_SLT : ICmpInst::ICMP_ULT;
    Margin = IsSigned ? APInt::getSignedMaxValue(BW) : APInt::getMaxValue(BW);
    Margin -= IV.Step;
    Margin += 1;
  } else {
    if (IV.Step.isMinSignedValue())
      return false;
    APInt AbsStep = IV.Step.abs();
    Pred = IsSigned ? ICmpInst::ICMP_SGT : ICmpInst::ICMP_UGT;
    Margin = IsSigned ? APInt::getSignedMinValue(BW) : APInt::getMinValue(BW);
    Margin += AbsStep;
    Margin -= 1;
  }
  const SCEV *MarginS = SE.getConstant(Margin);
  if (SE.isKnownPredicate(Pred, LimitS, MarginS))
    return true;
  return SE.isKnownPredicateAt(Pred, LimitS, MarginS, CtxI);
}

struct StripMineShape {
  BasicBlock *Preheader, *Header, *Latch, *ExitingBB, *ExitBB;
  BranchInst *ExitingBr;
  Value *ResumeIV;
  unsigned ExitSuccessorIdx;
  unsigned LimitOperandIdx;
  bool Increasing;
  bool Inclusive; // ContinuePredicate is *LE / *GE (runs one extra iteration)
  ICmpInst::Predicate ContinuePredicate;
  bool ExitUsesLatchValues;
};

// KEEP IN SYNC WITH SafepointCoverageVerifier.cpp's structural strip-mined
// shape check.
// Narrow preconditions kept simple so the CFG surgery is auditable:
// LoopSimplify form, a single primary counted exit whose conditional branch is
// ExitCmp (ordinary side exits are permitted alongside it), a relational
// predicate whose direction matches the step sign, and the IV phi compared
// directly against a loop-invariant limit.
std::optional<StripMineShape> checkStripMineShape(Loop *L, const IVInfo &IV,
                                                  ICmpInst *ExitCmp,
                                                  ScalarEvolution &SE,
                                                  bool AllowRuntimeVersioning) {
  // LoopSimplify form is defined as having a preheader, a single latch, and
  // dedicated exits, so getLoopPreheader()/getLoopLatch() are non-null here.
  if (!L->isLoopSimplifyForm())
    return std::nullopt;
  BasicBlock *Preheader = L->getLoopPreheader();
  BasicBlock *Header = L->getHeader();
  BasicBlock *Latch = L->getLoopLatch();

  // The surgery reroutes the preheader's branch into the new outer preheader.
  // LoopSimplify gives the preheader a single successor but not necessarily a
  // plain BranchInst terminator (a callbr could reach the header), so require
  // one here — before any mutation — rather than cast blindly mid-surgery.
  if (!isa<BranchInst>(Preheader->getTerminator()))
    return std::nullopt;

  // The surgery rewires the primary counted exit only. Any other exit edge
  // already leaves the current loop and, after wrapping, the strip-mined nest,
  // so it is not part of the long-running back-edge path that needs bounded
  // TTSP coverage; keep it and its exit state untouched. This mirrors C2
  // CountedLoop's "1 trip-counter exit path and maybe other exit paths"
  // contract.
  BasicBlock *ExitingBB = ExitCmp->getParent();
  SmallVector<BasicBlock *, 4> ExitingBBs;
  L->getExitingBlocks(ExitingBBs);
  if (!llvm::is_contained(ExitingBBs, ExitingBB))
    return std::nullopt;

  Value *LatchIV = IV.Phi->getIncomingValueForBlock(Latch);
  bool HeaderExit = ExitingBB == Header && IV.ComparedValue == IV.Phi;
  bool LatchNextExit = ExitingBB == Latch && IV.ComparedValue == LatchIV;
  auto LatchStep = getConstantAddStep(LatchIV, IV.Phi);
  if (!LatchStep || *LatchStep != IV.Step)
    return std::nullopt;
  if (LatchNextExit) {
    auto *LatchNextInst = dyn_cast<Instruction>(LatchIV);
    if (!LatchNextInst || LatchNextInst->getParent() != Latch)
      return std::nullopt;
  }
  // Guarded/skippable counted tests are unsafe for this transform: the clamped
  // inner limit might not execute every iteration, so the poll-free batch would
  // not be bounded.
  if (!HeaderExit && !LatchNextExit)
    return std::nullopt;
  if (HeaderExit) {
    // Header exits re-enter the header at batch boundaries before executing the
    // next real iteration. Keep the replayed header prefix side-effect-free.
    for (Instruction &I : *Header) {
      if (isa<PHINode>(I) || I.isTerminator() || &I == ExitCmp)
        continue;
      if (I.mayHaveSideEffects() || I.mayReadOrWriteMemory())
        return std::nullopt;
    }
  }

  auto *Br = dyn_cast<BranchInst>(ExitingBB->getTerminator());
  if (!Br || !Br->isConditional() || Br->getCondition() != ExitCmp)
    return std::nullopt;

  // The surgery rewrites ExitCmp's limit operand in place to the clamped
  // per-batch limit. If ExitCmp feeds anything other than this branch (a
  // select, another branch, a side-exit poll's deopt bundle), that user would
  // silently start reading the clamped limit instead of the real one. Only
  // rewrite a compare used solely by the exit branch. (EarlyCSE, now in the
  // strip-mining pipeline, can merge equal compares into a shared multi-use
  // value, so this is not hypothetical.)
  if (!ExitCmp->hasOneUse())
    return std::nullopt;

  unsigned ExitSuccIdx = 0;
  BasicBlock *ExitBB = nullptr;
  for (unsigned I = 0; I < Br->getNumSuccessors(); ++I) {
    if (!L->contains(Br->getSuccessor(I))) {
      if (ExitBB)
        return std::nullopt; // more than one exit edge from this branch
      ExitBB = Br->getSuccessor(I);
      ExitSuccIdx = I;
    }
  }
  if (!ExitBB)
    return std::nullopt;

  // Normalise branch polarity: reason in terms of the predicate that is true
  // when we should *continue* the inner batch, so exit-on-true and
  // exit-on-false shapes are handled on equal footing.
  bool ExitOnTrue = (ExitSuccIdx == 0);
  ICmpInst::Predicate ContinuePred =
      ExitOnTrue ? ICmpInst::getInversePredicate(ExitCmp->getPredicate())
                 : ExitCmp->getPredicate();

  unsigned LimitIdx;
  bool ComparedOnRHS = false;
  if (ExitCmp->getOperand(0) == IV.ComparedValue)
    LimitIdx = 1;
  else if (ExitCmp->getOperand(1) == IV.ComparedValue) {
    LimitIdx = 0;
    ComparedOnRHS = true;
  } else {
    return std::nullopt;
  }
  if (ComparedOnRHS)
    ContinuePred = ICmpInst::getSwappedPredicate(ContinuePred);

  bool CanonicalizedNE = ContinuePred == ICmpInst::ICMP_NE;
  if (CanonicalizedNE) {
    if (!IV.Step.abs().isOne())
      return std::nullopt;
    ContinuePred =
        IV.Step.isStrictlyPositive() ? ICmpInst::ICMP_SLT : ICmpInst::ICMP_SGT;
  }

  bool Increasing, Inclusive;
  switch (ContinuePred) {
  case ICmpInst::ICMP_SLT:
  case ICmpInst::ICMP_ULT:
    Increasing = true;
    Inclusive = false;
    break;
  case ICmpInst::ICMP_SLE:
  case ICmpInst::ICMP_ULE:
    Increasing = true;
    Inclusive = true;
    break;
  case ICmpInst::ICMP_SGT:
  case ICmpInst::ICMP_UGT:
    Increasing = false;
    Inclusive = false;
    break;
  case ICmpInst::ICMP_SGE:
  case ICmpInst::ICMP_UGE:
    Increasing = false;
    Inclusive = true;
    break;
  default:
    return std::nullopt;
  }
  if (Increasing != IV.Step.isStrictlyPositive())
    return std::nullopt; // step direction conflicts with predicate direction

  if (!L->isLoopInvariant(ExitCmp->getOperand(LimitIdx)))
    return std::nullopt;

  const SCEV *Start = IV.AR->getStart();
  const SCEV *Limit = SE.getSCEV(ExitCmp->getOperand(LimitIdx));
  // No-wrap legality, including the supported runtime-versioned case, is
  // checked after this structural normalization.
  if (CanonicalizedNE && HeaderExit) {
    ICmpInst::Predicate EntryPred =
        Increasing ? ICmpInst::ICMP_SLE : ICmpInst::ICMP_SGE;
    if (!SE.isLoopEntryGuardedByCond(L, EntryPred, Start, Limit))
      return std::nullopt;
  }

  if (LatchNextExit) {
    bool CanAddStableEntryGuard = AllowRuntimeVersioning && Inclusive &&
                                  ICmpInst::isSigned(ContinuePred) &&
                                  !IV.Step.isMinSignedValue() &&
                                  IV.Phi->getType()->isIntegerTy(32);
    if (!SE.isLoopEntryGuardedByCond(L, ContinuePred, Start, Limit) &&
        !CanAddStableEntryGuard)
      return std::nullopt;
  }

  return StripMineShape{Preheader,
                        Header,
                        Latch,
                        ExitingBB,
                        ExitBB,
                        Br,
                        LatchNextExit ? LatchIV : IV.Phi,
                        ExitSuccIdx,
                        LimitIdx,
                        Increasing,
                        Inclusive,
                        ContinuePred,
                        LatchNextExit};
}

// Back-edge polls owned by L (not a sub-loop) whose block dominates the latch.
// Dominance proves coverage only; relocation has separate memory and deopt
// state gates below.
SmallVector<CallInst *, 4> collectBackEdgePolls(Loop *L, LoopInfo &LI,
                                                DominatorTree &DT) {
  BasicBlock *Latch = L->getLoopLatch();
  SmallVector<CallInst *, 4> Out;
  for (BasicBlock *BB : L->blocks()) {
    if (LI.getLoopFor(BB) != L)
      continue;
    if (!DT.dominates(BB, Latch))
      continue;
    for (Instruction &I : *BB)
      if (isSafepointPoll(I))
        Out.push_back(cast<CallInst>(&I));
  }
  return Out;
}

// The value a header phi carries from the latch edge — the loop-carried "next"
// value (e.g. i.next, s.next). The frontend's back-edge deopt bundle captures
// these, not the phis themselves (the state is "resume at the next iteration").
PHINode *getHeaderPhiForLatchValue(Value *V, BasicBlock *Header,
                                   BasicBlock *Latch) {
  for (PHINode &Phi : Header->phis())
    if (Phi.getIncomingValueForBlock(Latch) == V)
      return &Phi;
  return nullptr;
}

bool isHeaderPhiLatchValue(Value *V, BasicBlock *Header, BasicBlock *Latch) {
  return getHeaderPhiForLatchValue(V, Header, Latch) != nullptr;
}

// Frontends may keep an unchanged local (notably `this`) live across a loop as
// a syntactic self recurrence. It is semantically invariant and can be lifted
// to the outer recurrence without changing the state observed at a batch
// boundary.
bool isLoopInvariantSelfRecurrence(PHINode *Phi, Loop *L, BasicBlock *Header,
                                   BasicBlock *Latch) {
  if (Phi->getParent() != Header || Phi->getIncomingValueForBlock(Latch) != Phi)
    return false;
  BasicBlock *Preheader = L->getLoopPreheader();
  Value *Initial =
      Preheader ? Phi->getIncomingValueForBlock(Preheader) : nullptr;
  return Initial && L->isLoopInvariant(Initial);
}

// The relocated poll represents the next iteration at a batch boundary. A raw
// header phi is normally current-iteration state, even when it also happens to
// be the latch input of another copy/swap recurrence. The one safe exception is
// an invariant self recurrence; reject every other header phi before testing
// latch-value matching.
bool deoptOperandsDescribeBatchBoundary(CallInst *P, Loop *L,
                                        BasicBlock *Header, BasicBlock *Latch) {
  auto OB = P->getOperandBundle(LLVMContext::OB_deopt);
  if (!OB)
    return true;
  for (const Use &U : OB->Inputs) {
    Value *V = U.get();
    if (L->isLoopInvariant(V))
      continue;
    if (auto *Phi = dyn_cast<PHINode>(V); Phi && Phi->getParent() == Header) {
      if (isLoopInvariantSelfRecurrence(Phi, L, Header, Latch))
        continue;
      return false;
    }
    if (isHeaderPhiLatchValue(V, Header, Latch))
      continue;
    return false;
  }
  return true;
}

bool memoryStateMatchesBackedge(CallInst *Poll, BasicBlock *Header,
                                BasicBlock *Latch, MemorySSA &MSSA) {
  auto *PollAccess = dyn_cast_or_null<MemoryDef>(MSSA.getMemoryAccess(Poll));
  MemoryPhi *HeaderMemory = MSSA.getMemoryAccess(Header);
  if (!PollAccess || !HeaderMemory ||
      HeaderMemory->getBasicBlockIndex(Latch) < 0)
    return false;
  return HeaderMemory->getIncomingValueForBlock(Latch) == PollAccess;
}

bool isRelocationHazard(const Instruction &I) {
  if (isAdjacencyTransparent(I))
    return false;
  return I.isAtomic() || isa<CallBase>(I) || I.mayHaveSideEffects();
}

// Walk backward from the latch and stop at the poll block. Since the poll block
// dominates the latch, this visits exactly the current-iteration paths that can
// still reach the backedge and ignores side-exit-only paths.
bool hasNoRelocationHazardAfterPoll(CallInst *Poll, Loop *L,
                                    BasicBlock *Latch) {
  BasicBlock *PollBB = Poll->getParent();
  SmallVector<BasicBlock *, 8> Worklist{Latch};
  SmallPtrSet<BasicBlock *, 16> Visited;
  bool ReachedPoll = false;

  while (!Worklist.empty()) {
    BasicBlock *BB = Worklist.pop_back_val();
    if (!Visited.insert(BB).second)
      continue;
    if (!L->contains(BB))
      return false;

    if (BB == PollBB) {
      ReachedPoll = true;
      for (auto It = std::next(Poll->getIterator()), End = BB->end(); It != End;
           ++It)
        if (isRelocationHazard(*It))
          return false;
      continue;
    }

    for (Instruction &I : *BB)
      if (isRelocationHazard(I))
        return false;

    bool HasLoopPredecessor = false;
    for (BasicBlock *Pred : predecessors(BB)) {
      if (!L->contains(Pred))
        return false;
      HasLoopPredecessor = true;
      Worklist.push_back(Pred);
    }
    if (!HasLoopPredecessor)
      return false;
  }
  return ReachedPoll;
}

struct HeaderPhiState {
  PHINode *Phi;
  Value *PreheaderValue;
  Value *LatchValue;
};

struct InclusiveExitPhiIncoming {
  PHINode *Phi;
  BasicBlock *ExitingBB;
  Value *IncomingValue;
};

struct InclusiveRuntimeGuard {
  ICmpInst::Predicate Predicate;
  ICmpInst::Predicate FirstIterationPredicate;
  APInt Bound;
  SmallVector<InclusiveExitPhiIncoming, 8> ExitPhiIncomings;
};

struct StripMinePlan {
  Loop *L;
  StripMineShape Shape;
  PHINode *IVPhi;
  ICmpInst *ExitCmp;
  CallInst *PollToMove;
  SmallVector<CallInst *, 4> AllPolls;
  SmallVector<PHINode *, 4> LiftedHeaderPhis;
  SmallVector<HeaderPhiState, 4> HeaderPhis;
  std::optional<InclusiveRuntimeGuard> RuntimeGuard;
  APInt AbsStepN;
  bool IsSigned;
  uint64_t ChunkIters;
  Value *InitVal;
  Value *Limit;
};

bool isSafeToVersionInclusiveLoop(Loop *L) {
  if (!L->isSafeToClone())
    return false;

  SmallVector<BasicBlock *, 4> ExitBlocks;
  L->getUniqueExitBlocks(ExitBlocks);
  for (BasicBlock *ExitBB : ExitBlocks) {
    auto It = ExitBB->getFirstNonPHIIt();
    if (It != ExitBB->end() && It->isEHPad())
      return false;
  }

  for (BasicBlock *BB : L->blocks()) {
    for (Instruction &I : *BB) {
      if (I.getType()->isTokenTy() && I.isUsedOutsideOfBlock(BB))
        return false;
      auto *CB = dyn_cast<CallBase>(&I);
      if (!CB)
        continue;
      if (isa<InvokeInst>(CB) || CB->isConvergent() ||
          CB->getAttributes()
              .getFnAttr(jeandle::Attribute::StatepointID)
              .isValid())
        return false;
    }
  }
  return true;
}

std::optional<StripMinePlan>
buildStripMinePlanWithIV(Loop *L, const IVInfo &IV, ICmpInst *ExitCmp,
                         CallInst *PollToMove, ArrayRef<CallInst *> AllPolls,
                         MemorySSA &MSSA, DominatorTree &DT,
                         ScalarEvolution &SE, bool AllowRuntimeVersioning) {
  auto MaybeShape =
      checkStripMineShape(L, IV, ExitCmp, SE, AllowRuntimeVersioning);
  if (!MaybeShape)
    return std::nullopt;
  StripMineShape Shape = *MaybeShape;

  if (!memoryStateMatchesBackedge(PollToMove, Shape.Header, Shape.Latch,
                                  MSSA) ||
      !hasNoRelocationHazardAfterPoll(PollToMove, L, Shape.Latch) ||
      !deoptOperandsDescribeBatchBoundary(PollToMove, L, Shape.Header,
                                          Shape.Latch))
    return std::nullopt;

  uint64_t N = jeandle::getSafepointChunkIters();
  if (N < 2)
    return std::nullopt;

  Type *Ty = IV.Phi->getType();
  Value *InitVal = IV.Phi->getIncomingValueForBlock(Shape.Preheader);
  Value *Limit = ExitCmp->getOperand(Shape.LimitOperandIdx);

  // Per-batch stride = |step| * Steps, where an inclusive predicate runs one
  // extra iteration so it advances Steps = N-1 (strict advances N). Compute it
  // in a wide value and bail unless it fits the IV type as a positive bound:
  // a narrow IV (or a chunk budget too large for the type) would otherwise
  // overflow the clamp arithmetic — truncating to a smaller or zero stride and,
  // at stride zero, spinning the outer loop forever. Bailing keeps such loops
  // un-mined (they retain their poll) rather than miscompiled.
  unsigned BitWidth = Ty->getIntegerBitWidth();
  uint64_t Steps = Shape.Inclusive ? N - 1 : N;
  bool IsSigned = ICmpInst::isSigned(Shape.ContinuePredicate);
  APInt WideStride = IV.Step.abs().zext(BitWidth + 64) *
                     APInt(BitWidth + 64, Steps, /*isSigned=*/false);
  if (WideStride.isZero() ||
      WideStride.getActiveBits() + (IsSigned ? 1 : 0) > BitWidth)
    return std::nullopt;
  APInt AbsStepN = WideStride.trunc(BitWidth);

  // The IV must not wrap inside a poll-free batch. The transform and verifier
  // prove the same real-limit range at loop entry, so an entry guard such as
  // `n < 1000` is usable but a body-only IV guard is not.
  const SCEV *LimitS = SE.getSCEV(Limit);
  const Instruction *LoopEntryCtx = Shape.Header->getTerminator();
  std::optional<InclusiveRuntimeGuard> RuntimeGuard;
  if (!Shape.Inclusive) {
    if (!canProveExclusiveNoWrap(IV, LimitS, LoopEntryCtx, IsSigned,
                                 Shape.Increasing, SE))
      return std::nullopt;
  } else {
    // The clamp and loop test use these operands separately. A SCEV range does
    // not make transitive uses of an undef-dependent SSA value agree.
    bool HasStableInit = isStableAcrossUses(InitVal);
    if (!HasStableInit ||
        !canProveInclusiveNoWrap(IV, Limit, LimitS, LoopEntryCtx, IsSigned,
                                 Shape.Increasing, SE)) {
      SmallVector<InclusiveExitPhiIncoming, 8> ExitPhiIncomings;
      SmallVector<BasicBlock *, 4> ExitBlocks;
      L->getUniqueExitBlocks(ExitBlocks);
      bool HasVersionableExits = L->hasDedicatedExits();
      for (BasicBlock *ExitBB : ExitBlocks) {
        for (PHINode &Phi : ExitBB->phis()) {
          for (unsigned I = 0; I < Phi.getNumIncomingValues(); ++I) {
            BasicBlock *ExitingBB = Phi.getIncomingBlock(I);
            if (!L->contains(ExitingBB)) {
              HasVersionableExits = false;
              break;
            }
            ExitPhiIncomings.push_back(
                {&Phi, ExitingBB, Phi.getIncomingValue(I)});
          }
          if (!HasVersionableExits)
            break;
        }
        if (!HasVersionableExits)
          break;
      }
      bool IsSupportedRuntimeShape =
          AllowRuntimeVersioning && HasVersionableExits &&
          isSafeToVersionInclusiveLoop(L) && IsSigned &&
          !IV.Step.isMinSignedValue() && IV.Phi->getType()->isIntegerTy(32) &&
          !Shape.Latch->getTerminator()->getMetadata(InclusiveSlowPathMD);
      if (!IsSupportedRuntimeShape)
        return std::nullopt;

      APInt GuardBound = Shape.Increasing
                             ? APInt::getSignedMaxValue(/*numBits=*/32)
                             : APInt::getSignedMinValue(/*numBits=*/32);
      if (Shape.Increasing) {
        GuardBound -= IV.Step;
        GuardBound += 1;
      } else {
        GuardBound += IV.Step.abs();
        GuardBound -= 1;
      }
      RuntimeGuard = InclusiveRuntimeGuard{
          Shape.Increasing ? ICmpInst::ICMP_SLT : ICmpInst::ICMP_SGT,
          Shape.Increasing ? ICmpInst::ICMP_SLE : ICmpInst::ICMP_SGE,
          GuardBound, std::move(ExitPhiIncomings)};
    }
  }

  // Every non-IV header phi is a recurrence that must continue across batches,
  // so all of them are threaded through the outer loop — not only those that
  // leak out via an exit phi.
  SmallVector<PHINode *, 4> LiftedHeaderPhis;
  for (PHINode &Phi : Shape.Header->phis())
    if (&Phi != IV.Phi)
      LiftedHeaderPhis.push_back(&Phi);

  // Pre-mutation validation: every value leaking out through an exit LCSSA phi
  // must be representable at the final outer-header exit. Header exits leak the
  // current header phis; latch exits leak the latch-carried next values.
  // Anything else is out of scope — bail before mutation.
  for (PHINode &Phi : Shape.ExitBB->phis()) {
    int Idx = Phi.getBasicBlockIndex(Shape.ExitingBB);
    if (Idx < 0)
      continue;
    Value *V = Phi.getIncomingValue(Idx);
    if (L->isLoopInvariant(V))
      continue;
    if (Shape.ExitUsesLatchValues) {
      if (V == Shape.ResumeIV)
        continue;
      PHINode *HPhi = getHeaderPhiForLatchValue(V, Shape.Header, Shape.Latch);
      if (!HPhi || HPhi == IV.Phi)
        return std::nullopt;
    } else {
      if (V == IV.Phi)
        continue;
      auto *HPhi = dyn_cast<PHINode>(V);
      if (!HPhi || HPhi->getParent() != Shape.Header)
        return std::nullopt;
    }
  }

  SmallVector<HeaderPhiState, 4> HeaderPhis;
  for (PHINode &Phi : Shape.Header->phis())
    HeaderPhis.push_back({&Phi, Phi.getIncomingValueForBlock(Shape.Preheader),
                          Phi.getIncomingValueForBlock(Shape.Latch)});

  return StripMinePlan{L,
                       Shape,
                       IV.Phi,
                       ExitCmp,
                       PollToMove,
                       SmallVector<CallInst *, 4>(AllPolls),
                       std::move(LiftedHeaderPhis),
                       std::move(HeaderPhis),
                       std::move(RuntimeGuard),
                       AbsStepN,
                       IsSigned,
                       N,
                       InitVal,
                       Limit};
}

std::optional<StripMinePlan>
buildStripMinePlan(Loop *L, LoopInfo &LI, DominatorTree &DT,
                   ScalarEvolution &SE, MemorySSA &MSSA,
                   bool AllowRuntimeVersioning = false) {
  if (!L->isInnermost() || hasConstantMaxBackedgeWithinBudget(*L, SE) ||
      !L->isLCSSAForm(DT) || !L->getLoopLatch() ||
      L->getLoopLatch()->getTerminator()->getMetadata(InclusiveSlowPathMD))
    return std::nullopt;

  SmallVector<CallInst *, 4> Polls = collectBackEdgePolls(L, LI, DT);
  if (Polls.empty())
    return std::nullopt;
  for (CallInst *P : Polls)
    if (isRequiredByAncestorLoop(P, *L, LI, DT, SE))
      return std::nullopt;

  CallInst *PollToMove = Polls.back();
  for (CallInst *P : Polls)
    if (hasCoverageMarker(*P)) {
      PollToMove = P;
      break;
    }

  for (ICmpInst *Cmp : collectExitConditions(L)) {
    auto IV = findIntInduction(L, Cmp, SE);
    if (!IV || !isAcceptablePredicate(Cmp->getPredicate()))
      continue;
    if (auto Plan =
            buildStripMinePlanWithIV(L, *IV, Cmp, PollToMove, Polls, MSSA, DT,
                                     SE, AllowRuntimeVersioning)) {
      if (AllowRuntimeVersioning && !Plan->RuntimeGuard)
        continue;
      return Plan;
    }
  }
  return std::nullopt;
}

bool stillStructurallyValid(const StripMinePlan &Plan, LoopInfo &LI,
                            DominatorTree &DT) {
  Loop *L = Plan.L;
  const StripMineShape &Shape = Plan.Shape;
  if (!L->isInnermost() || L->getHeader() != Shape.Header ||
      L->getLoopPreheader() != Shape.Preheader ||
      L->getLoopLatch() != Shape.Latch || LI.getLoopFor(Shape.Header) != L ||
      !L->contains(Shape.ExitingBB) || !L->isLCSSAForm(DT))
    return false;
  if (!Plan.PollToMove->getParent() ||
      LI.getLoopFor(Plan.PollToMove->getParent()) != L ||
      !DT.dominates(Plan.PollToMove->getParent(), Shape.Latch))
    return false;
  if (Plan.ExitCmp->getParent() != Shape.ExitingBB ||
      !Plan.ExitCmp->hasOneUse() ||
      Plan.ExitCmp->getOperand(Shape.LimitOperandIdx) != Plan.Limit ||
      Shape.ExitingBr->getParent() != Shape.ExitingBB ||
      Shape.ExitingBr->getCondition() != Plan.ExitCmp ||
      Shape.ExitingBr->getSuccessor(Shape.ExitSuccessorIdx) != Shape.ExitBB)
    return false;
  for (CallInst *P : Plan.AllPolls)
    if (!P->getParent() || LI.getLoopFor(P->getParent()) != L)
      return false;
  for (const HeaderPhiState &State : Plan.HeaderPhis) {
    int PreheaderIdx = State.Phi->getBasicBlockIndex(Shape.Preheader);
    int LatchIdx = State.Phi->getBasicBlockIndex(Shape.Latch);
    if (PreheaderIdx < 0 || LatchIdx < 0 ||
        State.Phi->getIncomingValue(PreheaderIdx) != State.PreheaderValue ||
        State.Phi->getIncomingValue(LatchIdx) != State.LatchValue)
      return false;
  }
  if (Plan.RuntimeGuard) {
    if (!isSafeToVersionInclusiveLoop(Plan.L))
      return false;
    for (const InclusiveExitPhiIncoming &Incoming :
         Plan.RuntimeGuard->ExitPhiIncomings) {
      int Idx = Incoming.Phi->getBasicBlockIndex(Incoming.ExitingBB);
      if (Idx < 0 ||
          Incoming.Phi->getIncomingValue(Idx) != Incoming.IncomingValue)
        return false;
    }
  }
  return true;
}

void replaceLoopUses(Loop *L, Value *From, Value *To) {
  if (From == To)
    return;
  for (BasicBlock *BB : L->blocks())
    for (Instruction &I : *BB)
      for (Use &U : I.operands())
        if (U.get() == From)
          U.set(To);
}

void applyInclusiveLoopVersioningPlan(StripMinePlan &Plan, LoopInfo &LI,
                                      DominatorTree &DT) {
  assert(Plan.RuntimeGuard && "versioning plan must carry a runtime guard");
  Loop *FastLoop = Plan.L;
  StripMineShape &Shape = Plan.Shape;
  Function *F = Shape.Header->getParent();

  BasicBlock *CheckBB = Shape.Preheader;
  BasicBlock *FastPreheader =
      SplitBlock(CheckBB, CheckBB->getTerminator(), &DT, &LI, nullptr,
                 Shape.Header->getName() + ".inclusive.fast.ph");

  IRBuilder<> CheckBuilder(CheckBB->getTerminator());
  Value *StableLimit =
      CheckBuilder.CreateFreeze(Plan.Limit, "inclusive.limit.fr");
  Value *StableInit =
      Plan.InitVal == Plan.Limit
          ? StableLimit
          : CheckBuilder.CreateFreeze(Plan.InitVal, "inclusive.start.fr");
  replaceLoopUses(FastLoop, Plan.Limit, StableLimit);
  replaceLoopUses(FastLoop, Plan.InitVal, StableInit);

  ValueToValueMapTy VMap;
  SmallVector<BasicBlock *, 8> SlowBlocks;
  Loop *SlowLoop =
      cloneLoopWithPreheader(FastPreheader, CheckBB, FastLoop, VMap,
                             ".inclusive.slow", &LI, &DT, SlowBlocks);
  remapInstructionsInBlocks(SlowBlocks, VMap);

  auto *SlowPreheader = cast<BasicBlock>(VMap[FastPreheader]);
  for (const InclusiveExitPhiIncoming &Incoming :
       Plan.RuntimeGuard->ExitPhiIncomings) {
    auto *SlowExitingBB = cast<BasicBlock>(VMap[Incoming.ExitingBB]);
    Value *SlowValue = Incoming.IncomingValue;
    if (auto It = VMap.find(SlowValue); It != VMap.end())
      SlowValue = It->second;
    Incoming.Phi->addIncoming(SlowValue, SlowExitingBB);
  }

  Instruction *OldTerm = CheckBB->getTerminator();
  BasicBlock *NoWrapCheckBB = BasicBlock::Create(
      F->getContext(), Shape.Header->getName() + ".inclusive.no_wrap.check", F,
      SlowPreheader);
  if (Loop *ParentL = FastLoop->getParentLoop())
    ParentL->addBasicBlockToLoop(NoWrapCheckBB, LI);
  IRBuilder<> B(NoWrapCheckBB);
  const InclusiveRuntimeGuard &Guard = *Plan.RuntimeGuard;
  Value *NoWrap =
      B.CreateICmp(Guard.Predicate, StableLimit,
                   ConstantInt::get(StableLimit->getType(), Guard.Bound),
                   "inclusive.no_wrap");
  B.CreateCondBr(NoWrap, FastPreheader, SlowPreheader);

  IRBuilder<> EntryBuilder(OldTerm);
  Value *FirstIteration =
      EntryBuilder.CreateICmp(Guard.FirstIterationPredicate, StableInit,
                              StableLimit, "inclusive.first_iteration");
  EntryBuilder.CreateCondBr(FirstIteration, NoWrapCheckBB, SlowPreheader);
  OldTerm->eraseFromParent();

  SlowLoop->getLoopLatch()->getTerminator()->setMetadata(
      InclusiveSlowPathMD, MDNode::get(F->getContext(), {}));

  // Both versions initially retain their original polls. Re-form dedicated
  // exits before the next pass recomputes MemorySSA and strip-mines only the
  // guarded version.
  DT.recalculate(*F);
  formDedicatedExitBlocks(SlowLoop, &DT, &LI, nullptr, true);
  formDedicatedExitBlocks(FastLoop, &DT, &LI, nullptr, true);
  DT.recalculate(*F);
}

void applyStripMinePlan(StripMinePlan &Plan, LoopInfo &LI, DominatorTree &DT,
                        ScalarEvolution &SE) {
  Loop *L = Plan.L;
  StripMineShape &Shape = Plan.Shape;
  PHINode *IVPhi = Plan.IVPhi;
  ICmpInst *ExitCmp = Plan.ExitCmp;
  CallInst *PollToMove = Plan.PollToMove;
  ArrayRef<CallInst *> AllPolls = Plan.AllPolls;
  ArrayRef<PHINode *> LiftedHeaderPhis = Plan.LiftedHeaderPhis;
  const APInt &AbsStepN = Plan.AbsStepN;
  bool IsSigned = Plan.IsSigned;
  uint64_t N = Plan.ChunkIters;
  Value *InitVal = Plan.InitVal;
  Value *Limit = Plan.Limit;

  Function *F = Shape.Header->getParent();
  LLVMContext &Ctx = F->getContext();
  Type *Ty = IVPhi->getType();

  BasicBlock *OuterPH =
      BasicBlock::Create(Ctx, Shape.Header->getName() + ".outer.ph", F);
  BasicBlock *OuterHeader =
      BasicBlock::Create(Ctx, Shape.Header->getName() + ".outer", F);
  BasicBlock *InnerEntry = BasicBlock::Create(
      Ctx, Shape.Header->getName() + ".outer.inner.entry", F);
  BasicBlock *OuterLatch =
      BasicBlock::Create(Ctx, Shape.Header->getName() + ".outer.latch", F);

  auto *PHBr = cast<BranchInst>(Shape.Preheader->getTerminator());
  for (unsigned I = 0; I < PHBr->getNumSuccessors(); ++I)
    if (PHBr->getSuccessor(I) == Shape.Header)
      PHBr->setSuccessor(I, OuterPH);

  IRBuilder<> B(OuterPH);
  B.CreateBr(OuterHeader);

  B.SetInsertPoint(OuterHeader);
  PHINode *OuterIV = B.CreatePHI(Ty, 2, "outer.iv");
  OuterIV->addIncoming(InitVal, OuterPH);
  SmallVector<PHINode *, 4> OuterReducPhis;
  for (PHINode *HPhi : LiftedHeaderPhis) {
    PHINode *OP = B.CreatePHI(HPhi->getType(), 2, HPhi->getName() + ".outer");
    OP->addIncoming(HPhi->getIncomingValueForBlock(Shape.Preheader), OuterPH);
    OuterReducPhis.push_back(OP);
  }
  // OuterCond is "continue the inner batch" so the br targets are fixed
  // regardless of the original branch polarity (ContinuePredicate encodes it).
  Value *OuterCond =
      B.CreateICmp(Shape.ContinuePredicate, OuterIV, Limit, "outer.cond");
  B.CreateCondBr(OuterCond, InnerEntry, Shape.ExitBB);

  // InnerEntry clamps the per-batch inner limit to min(batch end, real limit).
  // Saturating arithmetic handles IV-type extremes: an overflow saturates and
  // the cap compare then pins the inner limit to the real limit.
  B.SetInsertPoint(InnerEntry);
  Value *StepN = ConstantInt::get(Ty, AbsStepN);
  Intrinsic::ID SatID =
      Shape.Increasing ? (IsSigned ? Intrinsic::sadd_sat : Intrinsic::uadd_sat)
                       : (IsSigned ? Intrinsic::ssub_sat : Intrinsic::usub_sat);
  Value *BatchEnd =
      B.CreateBinaryIntrinsic(SatID, OuterIV, StepN,
                              /*FMFSource=*/nullptr, "outer.batch.end");
  Value *KeepEnd =
      B.CreateICmp(Shape.ContinuePredicate, BatchEnd, Limit, "outer.cap.cond");
  Value *InnerLimit =
      B.CreateSelect(KeepEnd, BatchEnd, Limit, "outer.inner.limit");
  B.CreateBr(Shape.Header);

  // Each batch resumes the recurrences from the outer progress.
  int PHIdx = IVPhi->getBasicBlockIndex(Shape.Preheader);
  IVPhi->setIncomingBlock(PHIdx, InnerEntry);
  IVPhi->setIncomingValue(PHIdx, OuterIV);
  for (size_t I = 0; I < LiftedHeaderPhis.size(); ++I) {
    int Ix = LiftedHeaderPhis[I]->getBasicBlockIndex(Shape.Preheader);
    LiftedHeaderPhis[I]->setIncomingBlock(Ix, InnerEntry);
    LiftedHeaderPhis[I]->setIncomingValue(Ix, OuterReducPhis[I]);
  }

  ExitCmp->setOperand(Shape.LimitOperandIdx, InnerLimit);
  Shape.ExitingBr->setSuccessor(Shape.ExitSuccessorIdx, OuterLatch);

  // OuterLatch captures the batch-boundary recurrences and carries the poll.
  B.SetInsertPoint(OuterLatch);
  PHINode *OuterIVNext = B.CreatePHI(Ty, 1, "outer.iv.next");
  OuterIVNext->addIncoming(Shape.ResumeIV, Shape.ExitingBB);
  SmallVector<PHINode *, 4> OuterReducNext;
  for (PHINode *HPhi : LiftedHeaderPhis) {
    PHINode *NP =
        B.CreatePHI(HPhi->getType(), 1, HPhi->getName() + ".outer.next");
    Value *Resume = Shape.ExitUsesLatchValues
                        ? HPhi->getIncomingValueForBlock(Shape.Latch)
                        : static_cast<Value *>(HPhi);
    NP->addIncoming(Resume, Shape.ExitingBB);
    OuterReducNext.push_back(NP);
  }
  BranchInst *OuterBr = B.CreateBr(OuterHeader);
  OuterIV->addIncoming(OuterIVNext, OuterLatch);
  for (size_t I = 0; I < OuterReducPhis.size(); ++I)
    OuterReducPhis[I]->addIncoming(OuterReducNext[I], OuterLatch);

  // Relocate the poll after the planning phase proved memory, control, and
  // deopt-state compatibility. Only latch-carried next values are remapped.
  // Skip a latch value that is loop-invariant (e.g. a phi whose latch operand
  // is a constant): it needs no remap, and keying Remap on it would spuriously
  // rewrite an unrelated but equal constant elsewhere in the deopt bundle.
  DenseMap<Value *, Value *> Remap;
  auto addRemap = [&](Value *LatchVal, Value *Outer) {
    if (!L->isLoopInvariant(LatchVal))
      Remap[LatchVal] = Outer;
  };
  addRemap(IVPhi->getIncomingValueForBlock(Shape.Latch), OuterIVNext);
  for (size_t I = 0; I < LiftedHeaderPhis.size(); ++I)
    addRemap(LiftedHeaderPhis[I]->getIncomingValueForBlock(Shape.Latch),
             OuterReducNext[I]);
  SmallVector<OperandBundleDef, 1> Bundles;
  if (auto OB = PollToMove->getOperandBundle(LLVMContext::OB_deopt)) {
    SmallVector<Value *, 8> Args;
    for (const Use &U : OB->Inputs) {
      Value *V = U.get();
      auto It = Remap.find(V);
      Args.push_back(It != Remap.end() ? It->second : V);
    }
    Bundles.emplace_back("deopt", Args);
  }
  CallBase *Anchor =
      CallBase::Create(PollToMove, Bundles, OuterBr->getIterator());
  Anchor->setMetadata(PollCoverageMD, MDNode::get(Ctx, {}));

  // The inner loop now runs poll-free, bounded to <= N iterations by the
  // clamped limit. SCEV can't see that bound through the select/saturating-add,
  // so mark the inner latch as a candidate for the structural coverage
  // verifier.
  L->getLoopLatch()->getTerminator()->setMetadata(jeandle::Metadata::StripMined,
                                                  MDNode::get(Ctx, {}));

  // Fix up the exit LCSSA phis: predecessor is now OuterHeader; a leaked IV
  // resolves to OuterIV, a leaked recurrence to its outer phi, an invariant
  // stays put. Resolve by the phi's incoming value (unique header phi -> outer
  // phi), so one header recurrence feeding several exit phis fixes up each of
  // them correctly.
  DenseMap<Value *, Value *> HeaderToOuter;
  DenseMap<Value *, Value *> LatchValueToOuter;
  for (size_t I = 0; I < LiftedHeaderPhis.size(); ++I)
    HeaderToOuter[LiftedHeaderPhis[I]] = OuterReducPhis[I];
  for (size_t I = 0; I < LiftedHeaderPhis.size(); ++I) {
    Value *LatchValue =
        LiftedHeaderPhis[I]->getIncomingValueForBlock(Shape.Latch);
    if (!L->isLoopInvariant(LatchValue))
      LatchValueToOuter[LatchValue] = OuterReducPhis[I];
  }
  for (PHINode &Phi : Shape.ExitBB->phis()) {
    int Idx = Phi.getBasicBlockIndex(Shape.ExitingBB);
    if (Idx < 0)
      continue;
    Value *V = Phi.getIncomingValue(Idx);
    Phi.setIncomingBlock(Idx, OuterHeader);
    if (L->isLoopInvariant(V))
      continue;
    if (V == IVPhi || V == Shape.ResumeIV)
      Phi.setIncomingValue(Idx, OuterIV);
    else if (auto It = HeaderToOuter.find(V); It != HeaderToOuter.end())
      Phi.setIncomingValue(Idx, It->second);
    else if (auto It = LatchValueToOuter.find(V); It != LatchValueToOuter.end())
      Phi.setIncomingValue(Idx, It->second);
  }

  // Inner polls are now covered by the relocated outer poll.
  for (CallInst *P : AllPolls)
    P->eraseFromParent();

  // LoopDeletionPrep handles finite empty loops separately. TODO(C2 parity):
  // C2 also has post-adjust cleanup for cases such as one-iteration loops,
  // disabled strip mining, and single outer iterations.

  // Maintain analyses by hand (no LPMUpdater): reparent the nest so it becomes
  // OuterL -> L, add the new blocks, then rebuild DT and drop stale SCEV. The
  // outer loop is not in this run's loop snapshot, so it is not revisited; its
  // poll carries the coverage marker against later passes.
  Loop *OuterL = LI.AllocateLoop();
  if (Loop *ParentL = L->getParentLoop()) {
    auto It = llvm::find(*ParentL, L);
    assert(It != ParentL->end() && "L not a child of its parent");
    ParentL->removeChildLoop(It);
    ParentL->addChildLoop(OuterL);
    ParentL->addBasicBlockToLoop(OuterPH, LI);
  } else {
    auto It = llvm::find(LI, L);
    assert(It != LI.end() && "L not a top-level loop");
    LI.removeLoop(It);
    LI.addTopLevelLoop(OuterL);
  }
  OuterL->addChildLoop(L);
  OuterL->addBasicBlockToLoop(OuterHeader, LI);
  OuterL->addBasicBlockToLoop(InnerEntry, LI);
  OuterL->addBasicBlockToLoop(OuterLatch, LI);
  for (BasicBlock *BB : L->blocks())
    OuterL->addBlockEntry(BB);

  DT.recalculate(*F);
  SE.forgetLoop(L);
  SE.forgetTopmostLoop(L);
  SE.forgetBlockAndLoopDispositions();

  LLVM_DEBUG(dbgs() << "  strip-mine: wrapped loop " << Shape.Header->getName()
                    << " (N=" << N << ", relocated poll to outer back-edge)\n");
}

} // end anonymous namespace

PreservedAnalyses SafepointElimination::run(Function &F,
                                            FunctionAnalysisManager &AM) {
  if (!EnableSafepointElim)
    return PreservedAnalyses::all();

  // Only compiled Java methods carry Jeandle safepoint polls; the module-level
  // named metadata mirrors the existing pattern in InsertGCBarriers.
  if (!F.getParent()->getNamedMetadata(
          jeandle::Metadata::JavaMethodCompilation))
    return PreservedAnalyses::all();

  // The compilation module also holds the template runtime helpers (instanceof
  // slow path, array clear, ...), tagged with the lower-phase attribute. They
  // are runtime stubs with their own bounded loops, not Java method bodies —
  // skip them. This also avoids paying loop analyses on every helper per
  // compile.
  if (F.hasFnAttribute(jeandle::Attribute::LowerPhase))
    return PreservedAnalyses::all();

  StringRef ModeName;
  switch (Mode) {
  case SafepointEliminationMode::Early:
    ModeName = "early";
    break;
  case SafepointEliminationMode::InclusiveLoopVersioning:
    ModeName = "inclusive-loop-versioning";
    break;
  case SafepointEliminationMode::StripMining:
    ModeName = "strip-mining";
    break;
  case SafepointEliminationMode::Cleanup:
    ModeName = "cleanup";
    break;
  case SafepointEliminationMode::LoopDeletionPrep:
    ModeName = "loop-deletion-prep";
    break;
  }
  LLVM_DEBUG(dbgs() << "SafepointElimination(" << ModeName
                    << "): " << F.getName() << "\n");

  if (Mode == SafepointEliminationMode::InclusiveLoopVersioning) {
    if (!EnableStripMining || !EnableInclusiveLoopVersioning)
      return PreservedAnalyses::all();

    auto &LI = AM.getResult<LoopAnalysis>(F);
    ReversePostOrderTraversal<const Function *> RPOT(&F);
    if (containsIrreducibleCFG<const BasicBlock *>(RPOT, LI))
      return PreservedAnalyses::all();

    auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
    auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
    auto &MSSA = AM.getResult<MemorySSAAnalysis>(F).getMSSA();
    SmallVector<Loop *, 8> Loops(LI.getLoopsInPreorder());
    SmallVector<StripMinePlan, 4> Plans;
    SmallPtrSet<BasicBlock *, 32> PlannedBlocks;

    // Prove the full strip-mining envelope, apart from the runtime no-wrap
    // fact, before cloning the first loop.
    for (Loop *L : llvm::reverse(Loops)) {
      auto Plan = buildStripMinePlan(L, LI, DT, SE, MSSA,
                                     /*AllowRuntimeVersioning=*/true);
      if (!Plan || !Plan->RuntimeGuard)
        continue;
      bool Overlaps = llvm::any_of(L->blocks(), [&](BasicBlock *BB) {
        return PlannedBlocks.contains(BB);
      });
      if (Overlaps)
        continue;
      for (BasicBlock *BB : L->blocks())
        PlannedBlocks.insert(BB);
      Plans.push_back(std::move(*Plan));
    }

    bool Changed = false;
    for (StripMinePlan &Plan : Plans) {
      if (!stillStructurallyValid(Plan, LI, DT))
        continue;
      applyInclusiveLoopVersioningPlan(Plan, LI, DT);
      Changed = true;
    }
    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }

  if (Mode == SafepointEliminationMode::LoopDeletionPrep) {
    auto &LI = AM.getResult<LoopAnalysis>(F);
    ReversePostOrderTraversal<const Function *> RPOT(&F);
    if (containsIrreducibleCFG<const BasicBlock *>(RPOT, LI)) {
      LLVM_DEBUG(dbgs() << "  irreducible CFG; skipping loop-deletion prep\n");
      return PreservedAnalyses::all();
    }

    auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
    auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
    SmallVector<Loop *, 8> Loops(LI.getLoopsInPreorder());
    SmallVector<EmptyLoopPollRemovalPlan, 4> Plans;

    // Complete every legality check before removing the first poll.
    for (Loop *L : llvm::reverse(Loops))
      if (auto Plan = buildEmptyLoopPollRemovalPlan(L, LI, DT, SE))
        Plans.push_back(std::move(*Plan));

    bool Changed = false;
    for (EmptyLoopPollRemovalPlan &Plan : Plans) {
      if (!stillStructurallyValid(Plan, LI, DT))
        continue;
      applyEmptyLoopPollRemovalPlan(Plan, LI, DT, SE);
      Changed = true;
    }
    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }

  if (Mode == SafepointEliminationMode::StripMining) {
    if (!EnableStripMining)
      return PreservedAnalyses::all();

    auto &LI = AM.getResult<LoopAnalysis>(F);
    ReversePostOrderTraversal<const Function *> RPOT(&F);
    if (containsIrreducibleCFG<const BasicBlock *>(RPOT, LI)) {
      LLVM_DEBUG(dbgs() << "  irreducible CFG; skipping strip mining\n");
      return PreservedAnalyses::all();
    }

    auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
    auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
    auto &MSSA = AM.getResult<MemorySSAAnalysis>(F).getMSSA();
    SmallVector<Loop *, 8> Loops(LI.getLoopsInPreorder());
    SmallVector<StripMinePlan, 4> Plans;
    SmallPtrSet<BasicBlock *, 32> PlannedBlocks;

    // Build every plan before the first mutation. Innermost target loops are
    // disjoint; enforce that property so a later plan never depends on stale
    // MemorySSA facts changed by an earlier apply.
    for (Loop *L : llvm::reverse(Loops)) {
      // Leave finite empty loops in their original shape. LoopDeletionPrep
      // later materializes deopt-only exit values and deletes each loop
      // atomically with its blocking poll.
      if (buildEmptyLoopPollRemovalPlan(L, LI, DT, SE))
        continue;
      auto Plan = buildStripMinePlan(L, LI, DT, SE, MSSA);
      if (!Plan)
        continue;
      bool Overlaps = llvm::any_of(L->blocks(), [&](BasicBlock *BB) {
        return PlannedBlocks.contains(BB);
      });
      if (Overlaps)
        continue;
      for (BasicBlock *BB : L->blocks())
        PlannedBlocks.insert(BB);
      Plans.push_back(std::move(*Plan));
    }

    bool Changed = false;
    for (StripMinePlan &Plan : Plans) {
      if (!stillStructurallyValid(Plan, LI, DT))
        continue;
      applyStripMinePlan(Plan, LI, DT, SE);
      Changed = true;
    }
    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }

  bool Changed = false;
  for (BasicBlock &BB : F)
    Changed |= collapseAdjacentPolls(BB);

  auto &LI = AM.getResult<LoopAnalysis>(F);

  // LoopInfo doesn't model irreducible cycles, so a natural loop's
  // latch-dominating poll or trip-count bound says nothing about a thread
  // spinning inside an irreducible sub-cycle: keep-one could delete that
  // cycle's polls while keeping a poll the cycle never reaches, and short-loop
  // deletion bounds only latch executions, not the cycle's internal spins.
  // Skip the loop transforms for the whole function; the block-local collapse
  // above is safe on any CFG.
  ReversePostOrderTraversal<const Function *> RPOT(&F);
  if (containsIrreducibleCFG<const BasicBlock *>(RPOT, LI)) {
    LLVM_DEBUG(dbgs() << "  irreducible CFG; skipping loop transforms\n");
  } else {
    auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
    auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);

    // Innermost-first, so an inner loop settles its own coverage before any
    // outer-loop decision looks at the nest.
    SmallVector<Loop *, 8> Loops(LI.getLoopsInPreorder());
    for (Loop *L : llvm::reverse(Loops)) {
      Changed |= keepOneLoopPoll(*L, LI, DT, SE);
      if (deleteShortLoopPolls(*L, LI, DT, SE))
        Changed = true;
    }
  }

  if (!Changed)
    return PreservedAnalyses::all();

  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}
