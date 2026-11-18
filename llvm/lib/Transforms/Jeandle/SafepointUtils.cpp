//===- SafepointUtils.cpp - Jeandle safepoint utilities ------------------===//
//
// Copyright (c) 2026, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Jeandle/SafepointUtils.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Jeandle/Attributes.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include <climits>

using namespace llvm;

static cl::opt<bool> EnableSafepointElimination(
    "jeandle-enable-safepoint-elim", cl::init(true),
    cl::desc("Master switch for Jeandle safepoint poll transformations."));

static cl::opt<uint64_t> LoopStripMiningIter(
    "jeandle-loop-strip-mining-iter", cl::init(1000),
    cl::desc("Short-loop poll-elimination budget and strip-mining batch size. "
             "0 disables strip mining."));

static cl::opt<bool> EnableInclusiveLoopVersioning(
    "jeandle-enable-inclusive-loop-versioning", cl::init(true),
    cl::desc("Clone supported runtime-risk inclusive loops behind a no-wrap "
             "guard so the safe version can be strip-mined."));

static cl::opt<uint64_t> SafepointMaxWork(
    "jeandle-safepoint-max-work", cl::Hidden, cl::init(1000000),
    cl::desc("Total safepoint analysis work per function pass invocation."));
static cl::opt<unsigned> SafepointMaxDepth(
    "jeandle-safepoint-max-depth", cl::Hidden, cl::init(64),
    cl::desc("Maximum loop depth for safepoint transformations."));

jeandle::SafepointWorkBudget::SafepointWorkBudget()
    : Remaining(SafepointMaxWork) {}

bool jeandle::SafepointWorkBudget::spend(uint64_t Work) {
  if (Exhausted || Work > Remaining) {
    Exhausted = true;
    return false;
  }
  Remaining -= Work;
  return true;
}

bool jeandle::SafepointWorkBudget::checkDepth(Loop &L) {
  unsigned Depth = 0;
  for (Loop *P = &L; P; P = P->getParentLoop()) {
    if (++Depth > SafepointMaxDepth || !spend()) {
      Exhausted = true;
      return false;
    }
  }
  return true;
}

bool jeandle::SafepointLoopAnalysis::checkLoop(Loop &L) {
  return Work.checkDepth(L) && Work.spend(L.getNumBlocks());
}

uint64_t llvm::jeandle::getLoopStripMiningIter() { return LoopStripMiningIter; }

bool llvm::jeandle::isSafepointEliminationEnabled() {
  return EnableSafepointElimination;
}

bool llvm::jeandle::isStripMiningEnabled() {
  return getLoopStripMiningIter() != 0;
}

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

bool llvm::jeandle::isSafepoint(const Instruction &I) {
  return isSafepointPoll(I) || isGuaranteedSafepointCall(I);
}

bool llvm::jeandle::isGuaranteedSafepointCall(const Instruction &I) {
  const auto *CB = dyn_cast<CallBase>(&I);
  if (!CB || isSafepointPoll(I) ||
      CB->getOperandBundle(LLVMContext::OB_deopt) == std::nullopt ||
      CB->hasFnAttr(Attribute::NotGuaranteedSafepoint) ||
      CB->hasFnAttr("gc-leaf-function"))
    return false;
  if (const Function *Callee = CB->getCalledFunction())
    if (Callee->hasFnAttribute(Attribute::NotGuaranteedSafepoint) ||
        Callee->hasFnAttribute("gc-leaf-function"))
      return false;
  return true;
}

bool llvm::jeandle::canProveExclusiveNoWrap(const SCEVAddRecExpr *AR,
                                            const APInt &Step,
                                            const SCEV *LimitS,
                                            const Instruction *CtxI,
                                            bool Signed, bool Increasing,
                                            ScalarEvolution &SE) {
  if (Step.abs().isOne())
    return true;
  if (Signed ? AR->hasNoSignedWrap() : Increasing && AR->hasNoUnsignedWrap())
    return true;

  unsigned BW = AR->getType()->getIntegerBitWidth();
  ICmpInst::Predicate Pred;
  APInt Bound;
  if (Increasing) {
    Pred = Signed ? ICmpInst::ICMP_SLE : ICmpInst::ICMP_ULE;
    Bound = Signed ? APInt::getSignedMaxValue(BW) : APInt::getMaxValue(BW);
    Bound -= Step;
    Bound += 1;
  } else {
    if (Step.isMinSignedValue())
      return false;
    APInt AbsStep = Step.abs();
    Pred = Signed ? ICmpInst::ICMP_SGE : ICmpInst::ICMP_UGE;
    Bound = Signed ? APInt::getSignedMinValue(BW) : APInt::getMinValue(BW);
    Bound += AbsStep;
    Bound -= 1;
  }
  return SE.isKnownPredicateAt(Pred, LimitS, SE.getConstant(Bound), CtxI);
}

bool llvm::jeandle::canProveInclusiveNoWrap(const SCEVAddRecExpr *AR,
                                            const APInt &Step, Value *Limit,
                                            const SCEV *LimitS,
                                            const Instruction *CtxI,
                                            bool Signed, bool Increasing,
                                            ScalarEvolution &SE) {
  if (!isGuaranteedNotToBeUndefOrPoison(Limit))
    return false;
  if (Signed ? AR->hasNoSignedWrap() : Increasing && AR->hasNoUnsignedWrap())
    return true;

  unsigned BW = AR->getType()->getIntegerBitWidth();
  ICmpInst::Predicate Pred;
  APInt Margin;
  if (Increasing) {
    Pred = Signed ? ICmpInst::ICMP_SLT : ICmpInst::ICMP_ULT;
    Margin = Signed ? APInt::getSignedMaxValue(BW) : APInt::getMaxValue(BW);
    Margin -= Step;
    Margin += 1;
  } else {
    if (Step.isMinSignedValue())
      return false;
    APInt AbsStep = Step.abs();
    Pred = Signed ? ICmpInst::ICMP_SGT : ICmpInst::ICMP_UGT;
    Margin = Signed ? APInt::getSignedMinValue(BW) : APInt::getMinValue(BW);
    Margin += AbsStep;
    Margin -= 1;
  }
  const SCEV *MarginS = SE.getConstant(Margin);
  return SE.isKnownPredicate(Pred, LimitS, MarginS) ||
         SE.isKnownPredicateAt(Pred, LimitS, MarginS, CtxI);
}

std::optional<APInt> llvm::jeandle::getConstantAddStep(Value *V, PHINode *Phi) {
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

bool llvm::jeandle::isStripMinedPoll(const Instruction &I) {
  if (!isSafepointPoll(I))
    return false;
  return cast<CallInst>(&I)->hasFnAttr(jeandle::Attribute::StripMinedPoll);
}

bool llvm::jeandle::isMarkedStripMinedInner(Loop &L) {
  Loop *Outer = L.getParentLoop();
  if (!Outer)
    return false;
  BasicBlock *OuterLatch = Outer->getLoopLatch();
  if (!OuterLatch)
    return false;
  return llvm::any_of(*OuterLatch, isStripMinedPoll);
}

bool llvm::jeandle::backedgeCountProvablyLessThan(Loop &L,
                                                  uint64_t ExclusiveLimit,
                                                  ScalarEvolution &SE) {
  const SCEV *MaxBTC = SE.getSymbolicMaxBackedgeTakenCount(&L);
  if (!isa<SCEVCouldNotCompute>(MaxBTC) && MaxBTC->getType()->isIntegerTy()) {
    unsigned BW = SE.getTypeSizeInBits(MaxBTC->getType());
    if (BW < 64 && !isUIntN(BW, ExclusiveLimit))
      return true;
    const SCEV *Limit = SE.getConstant(MaxBTC->getType(), ExclusiveLimit);
    if (SE.isKnownPredicate(ICmpInst::ICMP_ULT, MaxBTC, Limit) ||
        SE.isKnownPredicateAt(ICmpInst::ICMP_ULT, MaxBTC, Limit,
                              L.getHeader()->getTerminator()) ||
        SE.isLoopEntryGuardedByCond(&L, ICmpInst::ICMP_ULT, MaxBTC, Limit))
      return true;
  }

  const auto *ConstantMax =
      dyn_cast<SCEVConstant>(SE.getConstantMaxBackedgeTakenCount(&L));
  if (!ConstantMax)
    return false;
  const APInt &Value = ConstantMax->getAPInt();
  if (Value.getBitWidth() < 64 && !isUIntN(Value.getBitWidth(), ExclusiveLimit))
    return true;
  return Value.ult(APInt(Value.getBitWidth(), ExclusiveLimit));
}

// A defined no-wrap add/sub gives a mathematical distance from Start. For
// unsigned clamps, a min/max of a saturating batch end and the source limit
// can only shorten that distance.
static bool hasNonWrappingUpdate(Value *V, bool Signed, bool Increasing) {
  auto *BO = dyn_cast<BinaryOperator>(V);
  if (!BO || (BO->getOpcode() != Instruction::Add &&
              BO->getOpcode() != Instruction::Sub))
    return false;
  if (Signed)
    return BO->hasNoSignedWrap();
  if (!BO->hasNoUnsignedWrap())
    return false;
  auto *C = dyn_cast<ConstantInt>(BO->getOperand(1));
  if (Increasing && BO->getOpcode() == Instruction::Add) {
    if (!C)
      C = dyn_cast<ConstantInt>(BO->getOperand(0));
  } else if (Increasing || BO->getOpcode() != Instruction::Sub) {
    return false;
  }
  return C && !C->isNegative();
}

static bool
isProgressFromImpl(Value *V, Value *Base, bool Signed, bool Increasing,
                   ScalarEvolution &SE, unsigned Depth,
                   DenseMap<std::pair<Value *, unsigned>, bool> &Cache) {
  if (V == Base)
    return true;
  if (Depth == 8)
    return false;
  auto Key = std::make_pair(V, Depth);
  auto [It, Inserted] = Cache.try_emplace(Key, false);
  if (!Inserted)
    return It->second;
  // Cache failures as well as successes: merged PHI paths can otherwise
  // repeatedly traverse the same value graph.

  bool Result = [&]() {
    if (const auto *AR = dyn_cast<SCEVAddRecExpr>(SE.getSCEV(V))) {
      const auto *Step = dyn_cast<SCEVConstant>(AR->getStepRecurrence(SE));
      if (AR->isAffine() && AR->getStart() == SE.getSCEV(Base) && Step &&
          !Step->getAPInt().isZero() &&
          Step->getAPInt().isNegative() != Increasing) {
        bool NoWrap = Signed ? AR->hasNoSignedWrap()
                             : Increasing && AR->hasNoUnsignedWrap();
        if (auto *Phi = dyn_cast<PHINode>(V)) {
          if (Phi->getParent() == AR->getLoop()->getHeader())
            if (BasicBlock *Latch = AR->getLoop()->getLoopLatch())
              NoWrap |= hasNonWrappingUpdate(
                  Phi->getIncomingValueForBlock(Latch), Signed, Increasing);
        }
        if (NoWrap)
          return true;
      }
    }
    if (auto *Phi = dyn_cast<PHINode>(V))
      return llvm::all_of(Phi->incoming_values(), [&](Value *Incoming) {
        return isProgressFromImpl(Incoming, Base, Signed, Increasing, SE,
                                  Depth + 1, Cache);
      });
    if (auto *BO = dyn_cast<BinaryOperator>(V)) {
      if (BO->getOpcode() != Instruction::Add &&
          BO->getOpcode() != Instruction::Sub)
        return false;
      auto *C = dyn_cast<ConstantInt>(BO->getOperand(1));
      bool NoWrap = Signed ? BO->hasNoSignedWrap() : BO->hasNoUnsignedWrap();
      if (!C || !NoWrap || (!Signed && C->isNegative()))
        return false;
      bool Forward = BO->getOpcode() == Instruction::Add
                         ? C->getValue().isNegative() != Increasing
                         : BO->getOpcode() == Instruction::Sub &&
                               C->getValue().isNegative() == Increasing;
      return Forward && isProgressFromImpl(BO->getOperand(0), Base, Signed,
                                           Increasing, SE, Depth + 1, Cache);
    }
    return false;
  }();
  Cache[Key] = Result;
  return Result;
}

static bool isProgressFrom(Value *V, Value *Base, bool Signed, bool Increasing,
                           ScalarEvolution &SE) {
  DenseMap<std::pair<Value *, unsigned>, bool> Cache;
  return isProgressFromImpl(V, Base, Signed, Increasing, SE, 0, Cache);
}

static Value *getClampedDistance(Value *Limit, Value *Start, bool Signed,
                                 bool Increasing, ScalarEvolution &SE) {
  if (auto *BO = dyn_cast<BinaryOperator>(Limit)) {
    bool NoWrap = (BO->getOpcode() == Instruction::Add ||
                   BO->getOpcode() == Instruction::Sub) &&
                  (Signed ? BO->hasNoSignedWrap() : BO->hasNoUnsignedWrap());
    if (!NoWrap)
      return nullptr;
    if (Increasing && BO->getOpcode() == Instruction::Add) {
      if (isProgressFrom(Start, BO->getOperand(0), Signed, Increasing, SE))
        return BO->getOperand(1);
      if (isProgressFrom(Start, BO->getOperand(1), Signed, Increasing, SE))
        return BO->getOperand(0);
    }
    if (!Increasing && BO->getOpcode() == Instruction::Sub &&
        isProgressFrom(Start, BO->getOperand(0), Signed, Increasing, SE))
      return BO->getOperand(1);
  }
  if (Signed)
    return nullptr;
  auto *Sel = dyn_cast<SelectInst>(Limit);
  if (!Sel)
    return nullptr;
  auto *Cmp = dyn_cast<ICmpInst>(Sel->getCondition());
  if (!Cmp)
    return nullptr;
  Value *Cap = Sel->getTrueValue();
  Value *Other = Sel->getFalseValue();
  auto Pred = Cmp->getPredicate();
  if (Cmp->getOperand(0) == Other && Cmp->getOperand(1) == Cap)
    Pred = CmpInst::getSwappedPredicate(Pred);
  else if (Cmp->getOperand(0) != Cap || Cmp->getOperand(1) != Other)
    return nullptr;
  if (Increasing ? (Pred != ICmpInst::ICMP_ULT && Pred != ICmpInst::ICMP_ULE)
                 : (Pred != ICmpInst::ICMP_UGT && Pred != ICmpInst::ICMP_UGE))
    return nullptr;
  auto *Sat = dyn_cast<IntrinsicInst>(Cap);
  if (!Sat ||
      Sat->getIntrinsicID() !=
          (Increasing ? Intrinsic::uadd_sat : Intrinsic::usub_sat) ||
      !isProgressFrom(Start, Sat->getArgOperand(0), Signed, Increasing, SE))
    return nullptr;
  return Sat->getArgOperand(1);
}

bool llvm::jeandle::isBoundedStripMinedInner(Loop &L, ScalarEvolution &SE,
                                             DominatorTree &DT) {
  uint64_t Budget = getLoopStripMiningIter();
  if (!Budget || !L.isInnermost() || !isMarkedStripMinedInner(L))
    return false;
  if (backedgeCountProvablyLessThan(L, Budget, SE))
    return true;
  BasicBlock *Latch = L.getLoopLatch();
  BasicBlock *Preheader = L.getLoopPreheader();
  if (!Latch || !Preheader)
    return false;
  SmallVector<BasicBlock *, 4> Exiting;
  L.getExitingBlocks(Exiting);
  for (BasicBlock *BB : Exiting) {
    auto *Br = dyn_cast<BranchInst>(BB->getTerminator());
    if (!Br || !Br->isConditional() || !DT.dominates(BB, Latch))
      continue;
    auto *Cmp = dyn_cast<ICmpInst>(Br->getCondition());
    if (!Cmp)
      continue;
    auto Pred = Cmp->getPredicate();
    if (!L.contains(Br->getSuccessor(0)))
      Pred = CmpInst::getInversePredicate(Pred);
    for (PHINode &Phi : L.getHeader()->phis()) {
      if (!Phi.getType()->isIntegerTy())
        continue;
      const auto *AR = dyn_cast<SCEVAddRecExpr>(SE.getSCEV(&Phi));
      if (!AR || AR->getLoop() != &L || !AR->isAffine())
        continue;
      const auto *Step = dyn_cast<SCEVConstant>(AR->getStepRecurrence(SE));
      if (!Step || Step->getAPInt().isZero() ||
          Step->getAPInt().isMinSignedValue())
        continue;
      Value *Checked = Cmp->getOperand(0);
      Value *Limit = Cmp->getOperand(1);
      auto ContinuePred = Pred;
      auto IsChecked = [&](Value *V) {
        return V == &Phi || V == Phi.getIncomingValueForBlock(Latch);
      };
      if (!IsChecked(Checked)) {
        std::swap(Checked, Limit);
        ContinuePred = CmpInst::getSwappedPredicate(ContinuePred);
      }
      if (!IsChecked(Checked) || !L.isLoopInvariant(Limit))
        continue;
      // A next value must really be one step of this recurrence. Cyclic PHIs
      // and arbitrary latch-carried expressions are not induction proofs.
      unsigned Offset = Checked != &Phi;
      if (SE.getSCEV(Checked) != (Offset ? SE.getAddExpr(AR, Step) : AR))
        continue;
      bool Signed = ICmpInst::isSigned(ContinuePred);
      bool Increasing = !Step->getAPInt().isNegative();
      bool Inclusive;
      switch (ContinuePred) {
      case ICmpInst::ICMP_SLT:
      case ICmpInst::ICMP_ULT:
        if (!Increasing)
          continue;
        Inclusive = false;
        break;
      case ICmpInst::ICMP_SLE:
      case ICmpInst::ICMP_ULE:
        if (!Increasing)
          continue;
        Inclusive = true;
        break;
      case ICmpInst::ICMP_SGT:
      case ICmpInst::ICMP_UGT:
        if (Increasing)
          continue;
        Inclusive = false;
        break;
      case ICmpInst::ICMP_SGE:
      case ICmpInst::ICMP_UGE:
        if (Increasing)
          continue;
        Inclusive = true;
        break;
      default:
        continue;
      }
      Value *Start = Phi.getIncomingValueForBlock(Preheader);
      bool NoWrap = Signed ? AR->hasNoSignedWrap()
                           : Increasing && AR->hasNoUnsignedWrap();
      NoWrap |= hasNonWrappingUpdate(Phi.getIncomingValueForBlock(Latch),
                                     Signed, Increasing);
      // A modular add of a negative stride cannot carry LLVM's nuw flag.
      // For an unsigned countdown, prove no borrow directly: the first
      // update starts at or above |step|, and a continuation cannot take the
      // recurrence below the predicate's no-borrow margin.
      if (!NoWrap && !Signed && !Increasing) {
        const SCEV *Magnitude = SE.getConstant(Step->getAPInt().abs());
        const SCEV *Margin =
            SE.getConstant(Step->getAPInt().abs() - !Inclusive);
        NoWrap =
            SE.isLoopEntryGuardedByCond(&L, ICmpInst::ICMP_UGE,
                                        SE.getSCEV(Start), Magnitude) &&
            SE.isKnownPredicate(ICmpInst::ICMP_UGE, SE.getSCEV(Limit), Margin);
      }
      if (!NoWrap)
        continue;
      Value *Distance =
          getClampedDistance(Limit, Start, Signed, Increasing, SE);
      if (!Distance)
        continue;
      const SCEV *Dist = SE.getSCEV(Distance);
      if (Signed && SE.getSignedRangeMin(Dist).isNegative())
        continue;
      // At most N-1 true continuation tests. Use a wider type for the
      // multiplication so extreme budgets cannot truncate the proof.
      unsigned Width = Step->getAPInt().getBitWidth() + 65;
      APInt Allowed = Step->getAPInt().abs().zext(Width) *
                      APInt(Width, Budget - 1 + Offset);
      if (Inclusive) {
        if (Allowed.isZero())
          continue;
        --Allowed;
      }
      APInt Max =
          Signed ? SE.getSignedRangeMax(Dist) : SE.getUnsignedRangeMax(Dist);
      if (Max.zext(Width).ule(Allowed))
        return true;
    }
  }
  return false;
}

llvm::jeandle::LoopSafepointFacts
llvm::jeandle::LoopSafepointFacts::get(Loop &L, ScalarEvolution &SE) {
  LoopSafepointFacts Facts;
  uint64_t Budget = getLoopStripMiningIter();
  Facts.IsWithinBudget =
      Budget != 0 && backedgeCountProvablyLessThan(L, Budget, SE);
  // This numerical approximation accepts loops whose maximum backedge count
  // is strictly below INT_MAX. It intentionally does not infer countedness
  // from the induction-variable type or loop shape.
  Facts.IsWithinIntBackedgeLimit =
      backedgeCountProvablyLessThan(L, INT_MAX, SE);
  return Facts;
}

namespace {

// Cut the reverse search at a safepoint, and never cross the header into a
// previous iteration. A visited block is shared by all latches, so merged
// backedges and nested cycles do not multiply the work.
bool scanBackedgePaths(Loop &L, bool CallsOnly,
                       jeandle::SafepointWorkBudget &Work,
                       jeandle::RequiredPolls *Polls = nullptr) {
  SmallVector<BasicBlock *, 8> Worklist;
  L.getLoopLatches(Worklist);
  if (Worklist.empty())
    return false;
  SmallPtrSet<BasicBlock *, 32> Visited;
  bool Covered = true;
  while (!Worklist.empty()) {
    BasicBlock *BB = Worklist.pop_back_val();
    if (!Visited.insert(BB).second)
      continue;
    if (!Work.spend(BB->size() + pred_size(BB) + 1))
      return false;
    if (llvm::any_of(*BB, jeandle::isGuaranteedSafepointCall))
      continue;
    if (!CallsOnly) {
      CallInst *Poll = nullptr;
      for (Instruction &I : llvm::reverse(*BB))
        if (jeandle::isSafepointPoll(I)) {
          Poll = cast<CallInst>(&I);
          break;
        }
      if (Poll) {
        if (Polls)
          Polls->insert(Poll);
        continue;
      }
    }
    if (BB == L.getHeader()) {
      Covered = false;
      continue;
    }
    for (BasicBlock *Pred : predecessors(BB)) {
      if (L.contains(Pred))
        Worklist.push_back(Pred);
      else
        Covered = false;
    }
  }
  return Covered;
}

BasicBlock *getIDomBlock(DominatorTree &DT, BasicBlock *BB) {
  auto *Node = DT.getNode(BB);
  auto *IDom = Node ? Node->getIDom() : nullptr;
  return IDom ? IDom->getBlock() : nullptr;
}

} // namespace

jeandle::SafepointLoopAnalysis::Coverage &
jeandle::SafepointLoopAnalysis::getCoverage(Loop &L) {
  auto [It, Inserted] = CoverageCache.try_emplace(&L);
  if (Inserted && checkLoop(L)) {
    It->second.Calls = scanBackedgePaths(L, true, Work);
    It->second.Covered = It->second.Calls ||
                         scanBackedgePaths(L, false, Work, &It->second.Polls);
  }
  return It->second;
}

bool jeandle::SafepointLoopAnalysis::hasCoverage(Loop &L, bool CallsOnly) {
  auto &C = getCoverage(L);
  return !Work.exhausted() && (CallsOnly ? C.Calls : C.Covered);
}

const jeandle::RequiredPolls &
jeandle::SafepointLoopAnalysis::requiredPolls(Loop &L) {
  return getCoverage(L).Polls;
}

jeandle::LoopSafepointFacts jeandle::SafepointLoopAnalysis::getFacts(Loop &L) {
  auto [It, Inserted] = FactsCache.try_emplace(&L);
  if (Inserted && checkLoop(L)) {
    It->second = LoopSafepointFacts::get(L, SE);
    It->second.HasGuaranteedCallCoverage = hasCoverage(L, true);
    RequiredPolls Empty;
    It->second.HasOwnDominatingPoll = findKeepOne(L, LI, DT, Empty) != nullptr;
  }
  return It->second;
}

bool jeandle::AncestorPollRequirements::isRequired(const CallInst &Poll,
                                                   DominatorTree &DT) const {
  return !Complete || Polls.contains(&Poll);
}

jeandle::AncestorPollRequirements
jeandle::computeAncestorPollRequirements(Loop &L, SafepointLoopAnalysis &A) {
  AncestorPollRequirements Requirements;
  if (!A.checkLoop(L)) {
    Requirements.Complete = false;
    return Requirements;
  }
  for (Loop *Ancestor = L.getParentLoop(); Ancestor;
       Ancestor = Ancestor->getParentLoop()) {
    auto Facts = A.getFacts(*Ancestor);
    if ((Ancestor->isInnermost() && Facts.IsWithinBudget) ||
        (!isStripMiningEnabled() && Facts.IsWithinIntBackedgeLimit) ||
        Facts.HasGuaranteedCallCoverage || Facts.HasOwnDominatingPoll)
      continue;
    const RequiredPolls &Polls = A.requiredPolls(*Ancestor);
    if (!A.Work.spend(Polls.size())) {
      Requirements.Complete = false;
      return Requirements;
    }
    for (CallInst *Poll : Polls)
      Requirements.Polls.insert(Poll);
  }
  Requirements.Complete = !A.Work.exhausted();
  return Requirements;
}

void jeandle::analyzeLoop(Loop &L, SafepointLoopAnalysis &A,
                          SmallPtrSetImpl<Loop *> &HasSfpt,
                          RequiredPolls &Required) {
  if (A.hasCoverage(L, true)) {
    HasSfpt.insert(&L);
    return;
  }
  if (A.getFacts(L).HasOwnDominatingPoll)
    return;
  for (CallInst *Poll : A.requiredPolls(L))
    Required.insert(Poll);
}

// Phase 2 keeper selection (C2 remove_safepoints keep_one idom-path keeper).
// The latch-closest poll this loop owns on the dominator chain, preferring one
// required by an ancestor (it must survive anyway); otherwise any dominating
// poll. Returns null if the loop has no single-latch keeper.
CallInst *llvm::jeandle::findKeepOne(Loop &L, LoopInfo &LI, DominatorTree &DT,
                                     const RequiredPolls &Required) {
  BasicBlock *Latch = L.getLoopLatch();
  if (!Latch)
    return nullptr;
  // Prefer an ancestor-required keeper; otherwise take any dominating poll.
  // Walk the dominator chain latch-up through the header (a header poll is a
  // valid keeper — the header dominates the latch).
  for (bool RequiredOnly : {true, false}) {
    for (BasicBlock *BB = Latch; BB && L.contains(BB);
         BB = getIDomBlock(DT, BB)) {
      if (LI.getLoopFor(BB) != &L)
        continue;
      for (Instruction &I : llvm::reverse(*BB)) {
        if (!jeandle::isSafepointPoll(I))
          continue;
        auto *P = cast<CallInst>(&I);
        if (RequiredOnly && !Required.contains(P))
          continue;
        return P;
      }
    }
  }
  return nullptr;
}

static bool memoryStateMatchesBackedge(CallInst *Poll, Loop *L,
                                       BasicBlock *Header, BasicBlock *Latch,
                                       MemorySSA &MSSA) {
  MemoryAccess *PollAccess = MSSA.getMemoryAccess(Poll);
  if (!PollAccess)
    return false;

  assert(isa<MemoryDef>(PollAccess) && "safepoint poll must be a MemoryDef");

  MemoryPhi *HeaderMemory = MSSA.getMemoryAccess(Header);
  if (HeaderMemory) {
    if (HeaderMemory->getBasicBlockIndex(Latch) < 0)
      return false;
    return HeaderMemory->getIncomingValueForBlock(Latch) == PollAccess;
  }

  // With no loop-header MemoryPhi, only a MemoryUse can be loop-invariant.
  return false;
}

static bool isUnmodeledRelocationHazard(const Instruction &I, MemorySSA &MSSA) {
  if (I.isDebugOrPseudoInst() || I.isLifetimeStartOrEnd())
    return false;
  return I.mayHaveSideEffects() && !MSSA.getMemoryAccess(&I);
}

// Walk backward from the latch and stop at the poll block. Since the poll block
// dominates the latch, this visits exactly the current-iteration paths that can
// still reach the backedge and ignores side-exit-only paths.
static bool hasNoUnmodeledRelocationHazardAfterPoll(CallInst *Poll, Loop *L,
                                                    BasicBlock *Latch,
                                                    MemorySSA &MSSA) {
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
        if (isUnmodeledRelocationHazard(*It, MSSA))
          return false;
      continue;
    }

    for (Instruction &I : *BB)
      if (isUnmodeledRelocationHazard(I, MSSA))
        return false;

    for (BasicBlock *Pred : predecessors(BB)) {
      if (!L->contains(Pred))
        return false;
      Worklist.push_back(Pred);
    }
  }
  return ReachedPoll;
}

bool llvm::jeandle::canRelocateSafepointPoll(CallInst &Poll, Loop &L,
                                             MemorySSA &MSSA) {
  BasicBlock *Latch = L.getLoopLatch();
  return Latch && L.contains(Poll.getParent()) &&
         MSSA.getDomTree().dominates(&Poll, Latch->getTerminator()) &&
         memoryStateMatchesBackedge(&Poll, &L, L.getHeader(), Latch, MSSA) &&
         hasNoUnmodeledRelocationHazardAfterPoll(&Poll, &L, Latch, MSSA);
}
