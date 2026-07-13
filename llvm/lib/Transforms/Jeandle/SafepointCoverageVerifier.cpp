//===- SafepointCoverageVerifier.cpp - Safepoint Coverage Check -----------===//
//
// Copyright (c) 2026, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// Verifies the invariant SafepointElimination must preserve: every loop
/// either has every backedge covered by a poll whose block dominates the
/// corresponding latch, or has a SCEV-provable trip count within the chunk
/// budget. A violation means a thread inside the loop may not reach a safepoint
/// in bounded time.
///
/// The check is sufficient, not necessary: a dominating poll inside a sub-loop
/// counts (every enclosing iteration passes through it). False positives must
/// be fixed by whitelisting the legal shape here, not by weakening the
/// transform.
///
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Jeandle/SafepointCoverageVerifier.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Jeandle/Attributes.h"
#include "llvm/IR/Jeandle/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Jeandle/SafepointElimination.h"

using namespace llvm;

#define DEBUG_TYPE "safepoint-elimination"

using jeandle::SafepointCoverageCheck;

namespace {

static bool blockHasSafepointPoll(BasicBlock *BB) {
  for (Instruction &I : *BB)
    if (jeandle::isSafepointPoll(I))
      return true;
  return false;
}

static std::optional<APInt> getConstantStepFromLatchValue(Value *V,
                                                          PHINode *Phi) {
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

static std::optional<bool> isIncreasingPredicate(ICmpInst::Predicate Pred) {
  switch (Pred) {
  case ICmpInst::ICMP_SLT:
  case ICmpInst::ICMP_ULT:
  case ICmpInst::ICMP_SLE:
  case ICmpInst::ICMP_ULE:
    return true;
  case ICmpInst::ICMP_SGT:
  case ICmpInst::ICMP_UGT:
  case ICmpInst::ICMP_SGE:
  case ICmpInst::ICMP_UGE:
    return false;
  default:
    return std::nullopt;
  }
}

static bool isInclusivePredicate(ICmpInst::Predicate Pred) {
  return Pred == ICmpInst::ICMP_SLE || Pred == ICmpInst::ICMP_ULE ||
         Pred == ICmpInst::ICMP_SGE || Pred == ICmpInst::ICMP_UGE;
}

static Intrinsic::ID expectedSatIntrinsic(bool Increasing, bool Signed) {
  if (Increasing)
    return Signed ? Intrinsic::sadd_sat : Intrinsic::uadd_sat;
  return Signed ? Intrinsic::ssub_sat : Intrinsic::usub_sat;
}

// KEEP IN SYNC WITH SafepointElimination.cpp's no-wrap helpers. The verifier
// checks the transformed loop: `InnerLimit = min/max(batchEnd, RealLimit)`, so
// proving the real limit is far enough from the type edge also proves the
// clamped inner limit cannot let the latch add wrap inside a poll-free batch.
static bool canProveExclusiveNoWrap(const SCEVAddRecExpr *AR, const APInt &Step,
                                    const SCEV *LimitS, const Instruction *CtxI,
                                    bool Signed, bool Increasing,
                                    ScalarEvolution &SE) {
  if (Step.abs().isOne())
    return true;
  if (Signed ? AR->hasNoSignedWrap() : AR->hasNoUnsignedWrap())
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

static bool isStableAcrossUsesImpl(const Value *V,
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

static bool isStableAcrossUses(const Value *V) {
  SmallPtrSet<const Value *, 8> Visiting;
  return isStableAcrossUsesImpl(V, Visiting);
}

// KEEP IN SYNC WITH SafepointElimination.cpp's no-wrap helpers. Inclusive tests
// execute the boundary value itself, so they require the real limit to be one
// step farther from the type edge than exclusive tests.
static bool canProveInclusiveNoWrap(const SCEVAddRecExpr *AR, const APInt &Step,
                                    Value *Limit, const SCEV *LimitS,
                                    const Instruction *CtxI, bool Signed,
                                    bool Increasing, ScalarEvolution &SE) {
  if (!isStableAcrossUses(Limit))
    return false;
  if (Signed ? AR->hasNoSignedWrap() : AR->hasNoUnsignedWrap())
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
  if (SE.isKnownPredicate(Pred, LimitS, MarginS))
    return true;
  return SE.isKnownPredicateAt(Pred, LimitS, MarginS, CtxI);
}

// KEEP IN SYNC WITH SafepointElimination.cpp's accepted strip-mining shape.
static bool isStructurallyStripMinedInner(Loop &L, DominatorTree &DT,
                                          ScalarEvolution &SE) {
  BasicBlock *Latch = L.getLoopLatch();
  Loop *Outer = L.getParentLoop();
  if (!Latch || !Outer)
    return false;

  BasicBlock *OuterHeader = Outer->getHeader();
  BasicBlock *OuterLatch = Outer->getLoopLatch();
  BasicBlock *InnerPreheader = L.getLoopPreheader();
  if (!OuterHeader || !OuterLatch || !InnerPreheader)
    return false;
  if (!Outer->contains(InnerPreheader) || L.contains(InnerPreheader))
    return false;
  if (!blockHasSafepointPoll(OuterLatch))
    return false;

  auto *PreBr = dyn_cast<BranchInst>(InnerPreheader->getTerminator());
  if (!PreBr || !PreBr->isUnconditional() ||
      PreBr->getSuccessor(0) != L.getHeader())
    return false;

  SmallVector<BasicBlock *, 4> ExitingBBs;
  L.getExitingBlocks(ExitingBBs);
  BasicBlock *PrimaryExiting = nullptr;
  BranchInst *PrimaryBr = nullptr;
  unsigned OuterLatchSuccIdx = 0;
  for (BasicBlock *BB : ExitingBBs) {
    bool ExitsToOuterLatch = false;
    bool HasOtherExit = false;
    Instruction *Term = BB->getTerminator();
    for (unsigned I = 0, E = Term->getNumSuccessors(); I < E; ++I) {
      BasicBlock *Succ = Term->getSuccessor(I);
      if (L.contains(Succ))
        continue;
      if (Succ == OuterLatch) {
        ExitsToOuterLatch = true;
        OuterLatchSuccIdx = I;
      } else if (!Outer->contains(Succ)) {
        HasOtherExit = true;
      } else {
        return false;
      }
    }
    if (!ExitsToOuterLatch) {
      if (!HasOtherExit)
        return false;
      continue;
    }
    if (HasOtherExit)
      return false;
    if (PrimaryExiting)
      return false;
    PrimaryExiting = BB;
    PrimaryBr = dyn_cast<BranchInst>(Term);
  }
  if (!PrimaryExiting)
    return false;
  if (!PrimaryBr || !PrimaryBr->isConditional())
    return false;

  auto *InnerCmp = dyn_cast<ICmpInst>(PrimaryBr->getCondition());
  if (!InnerCmp)
    return false;

  ICmpInst::Predicate ContinuePred =
      OuterLatchSuccIdx == 0
          ? ICmpInst::getInversePredicate(InnerCmp->getPredicate())
          : InnerCmp->getPredicate();

  PHINode *IVPhi = nullptr;
  Value *ComparedValue = nullptr;
  SelectInst *InnerLimit = nullptr;
  bool ComparedOnRHS = false;
  for (PHINode &Phi : L.getHeader()->phis()) {
    Value *LatchValue = Phi.getIncomingValueForBlock(Latch);
    if (InnerCmp->getOperand(0) == &Phi ||
        InnerCmp->getOperand(0) == LatchValue) {
      IVPhi = &Phi;
      ComparedValue = InnerCmp->getOperand(0);
      InnerLimit = dyn_cast<SelectInst>(InnerCmp->getOperand(1));
      break;
    }
    if (InnerCmp->getOperand(1) == &Phi ||
        InnerCmp->getOperand(1) == LatchValue) {
      IVPhi = &Phi;
      ComparedValue = InnerCmp->getOperand(1);
      InnerLimit = dyn_cast<SelectInst>(InnerCmp->getOperand(0));
      ComparedOnRHS = true;
      break;
    }
  }
  if (!IVPhi || !InnerLimit)
    return false;
  if (ComparedOnRHS)
    ContinuePred = ICmpInst::getSwappedPredicate(ContinuePred);

  Value *LatchValue = IVPhi->getIncomingValueForBlock(Latch);
  bool HeaderExit = PrimaryExiting == L.getHeader() && ComparedValue == IVPhi;
  bool LatchNextExit = PrimaryExiting == Latch && ComparedValue == LatchValue;
  if (LatchNextExit) {
    auto *LatchNextInst = dyn_cast<Instruction>(LatchValue);
    if (!LatchNextInst || LatchNextInst->getParent() != Latch)
      return false;
  }
  // The primary counted exit must still be the mandatory test that bounds each
  // poll-free chunk. Other side exits were checked above to leave the whole
  // strip-mined nest and therefore do not contribute to the coverage proof.
  if (!HeaderExit && !LatchNextExit)
    return false;

  auto Step = getConstantStepFromLatchValue(LatchValue, IVPhi);
  if (!Step)
    return false;
  auto *AR = dyn_cast<SCEVAddRecExpr>(SE.getSCEV(IVPhi));
  if (!AR || AR->getLoop() != &L || !AR->isAffine())
    return false;
  if (ContinuePred == ICmpInst::ICMP_NE) {
    if (!Step->abs().isOne())
      return false;
    ContinuePred =
        Step->isStrictlyPositive() ? ICmpInst::ICMP_SLT : ICmpInst::ICMP_SGT;
  }

  auto Increasing = isIncreasingPredicate(ContinuePred);
  if (!Increasing)
    return false;
  bool Signed = ICmpInst::isSigned(ContinuePred);

  if (Step->isStrictlyPositive() != *Increasing)
    return false;

  auto *CapCmp = dyn_cast<ICmpInst>(InnerLimit->getCondition());
  if (!CapCmp || CapCmp->getPredicate() != ContinuePred)
    return false;
  Value *BatchEnd = InnerLimit->getTrueValue();
  Value *RealLimit = InnerLimit->getFalseValue();
  if (CapCmp->getOperand(0) != BatchEnd || CapCmp->getOperand(1) != RealLimit)
    return false;
  const SCEV *RealLimitS = SE.getSCEV(RealLimit);
  if (isInclusivePredicate(ContinuePred)) {
    if (!canProveInclusiveNoWrap(AR, *Step, RealLimit, RealLimitS, InnerLimit,
                                 Signed, *Increasing, SE))
      return false;
  } else if (!canProveExclusiveNoWrap(AR, *Step, RealLimitS, InnerLimit, Signed,
                                      *Increasing, SE)) {
    return false;
  }

  auto *OuterBr = dyn_cast<BranchInst>(OuterHeader->getTerminator());
  if (!OuterBr || !OuterBr->isConditional())
    return false;
  if (OuterBr->getSuccessor(0) != InnerPreheader)
    return false;
  auto *OuterCmp = dyn_cast<ICmpInst>(OuterBr->getCondition());
  if (!OuterCmp || OuterCmp->getPredicate() != ContinuePred)
    return false;
  auto *OuterIV = dyn_cast<PHINode>(OuterCmp->getOperand(0));
  if (!OuterIV || OuterIV->getParent() != OuterHeader ||
      OuterCmp->getOperand(1) != RealLimit)
    return false;
  if (IVPhi->getIncomingValueForBlock(InnerPreheader) != OuterIV)
    return false;
  if (isInclusivePredicate(ContinuePred)) {
    // The transform freezes the initial outer IV before it is reused by the
    // batch clamp and inner recurrence; require that structural fact here.
    BasicBlock *OuterPreheader = Outer->getLoopPreheader();
    if (!OuterPreheader)
      return false;
    Value *OuterInit = OuterIV->getIncomingValueForBlock(OuterPreheader);
    if (!OuterInit || !isStableAcrossUses(OuterInit))
      return false;
  }

  auto *Sat = dyn_cast<IntrinsicInst>(BatchEnd);
  auto *StepN = Sat ? dyn_cast<ConstantInt>(Sat->getArgOperand(1)) : nullptr;
  if (!Sat || !StepN ||
      Sat->getIntrinsicID() != expectedSatIntrinsic(*Increasing, Signed))
    return false;
  if (Sat->getArgOperand(0) != OuterIV)
    return false;

  uint64_t Chunk = jeandle::getSafepointChunkIters();
  if (Chunk == 0 || (isInclusivePredicate(ContinuePred) && Chunk == 1))
    return false;
  unsigned WideBits = StepN->getValue().getBitWidth() + 64;
  APInt Allowed = Step->abs().zext(WideBits);
  Allowed *=
      APInt(WideBits, isInclusivePredicate(ContinuePred) ? Chunk - 1 : Chunk,
            /*isSigned=*/false);
  APInt Actual = StepN->getValue().zext(WideBits);
  if (Actual.isZero() || Actual.ugt(Allowed))
    return false;

  auto *OuterBackBr = dyn_cast<BranchInst>(OuterLatch->getTerminator());
  if (!OuterBackBr || !OuterBackBr->isUnconditional() ||
      OuterBackBr->getSuccessor(0) != OuterHeader)
    return false;
  Value *OuterIVNext = OuterIV->getIncomingValueForBlock(OuterLatch);
  auto *OuterIVNextPhi = dyn_cast<PHINode>(OuterIVNext);
  if (!OuterIVNextPhi || OuterIVNextPhi->getParent() != OuterLatch)
    return false;
  Value *ExpectedResume =
      LatchNextExit ? LatchValue : static_cast<Value *>(IVPhi);
  return OuterIVNextPhi->getIncomingValueForBlock(PrimaryExiting) ==
         ExpectedResume;
}

} // namespace

static cl::opt<SafepointCoverageCheck> CoverageCheck(
    "jeandle-verify-safepoint-coverage",
    cl::values(clEnumValN(SafepointCoverageCheck::Off, "off",
                          "Do not run the safepoint coverage verifier"),
               clEnumValN(SafepointCoverageCheck::Warn, "warn",
                          "Report all coverage violations without aborting"),
               clEnumValN(SafepointCoverageCheck::Fatal, "fatal",
                          "Abort the compile on any coverage violation")),
    cl::init(SafepointCoverageCheck::Off),
    cl::desc("Safepoint coverage verifier mode."));

SafepointCoverageCheck llvm::jeandle::getSafepointCoverageCheck() {
  return CoverageCheck;
}

static bool isLoopCovered(Loop &L, DominatorTree &DT, ScalarEvolution &SE) {
  SmallVector<BasicBlock *, 4> Latches;
  L.getLoopLatches(Latches);
  if (Latches.empty())
    return false;

  // SCEV can't recover the strip-mined inner bound through the clamped select,
  // so validate the wrapper shape behind the transform-authored marker.
  if (Latches.size() == 1 &&
      Latches.front()->getTerminator()->getMetadata(
          jeandle::Metadata::StripMined) &&
      isStructurallyStripMinedInner(L, DT, SE))
    return true;

  bool AllLatchesCovered = true;
  for (BasicBlock *Latch : Latches) {
    bool LatchCovered = false;
    for (BasicBlock *BB : L.blocks()) {
      if (DT.dominates(BB, Latch) && blockHasSafepointPoll(BB)) {
        LatchCovered = true;
        break;
      }
    }
    if (!LatchCovered) {
      AllLatchesCovered = false;
      break;
    }
  }
  if (AllLatchesCovered)
    return true;

  const auto *MaxC =
      dyn_cast<SCEVConstant>(SE.getConstantMaxBackedgeTakenCount(&L));
  return MaxC && MaxC->getAPInt().ule(jeandle::getSafepointChunkIters());
}

PreservedAnalyses SafepointCoverageVerifier::run(Function &F,
                                                 FunctionAnalysisManager &AM) {
  if (CoverageCheck == SafepointCoverageCheck::Off)
    return PreservedAnalyses::all();

  if (!F.getParent()->getNamedMetadata(
          jeandle::Metadata::JavaMethodCompilation))
    return PreservedAnalyses::all();

  // Template runtime helpers (lower-phase attribute) have their own bounded
  // loops that legitimately run without polls; they are not Java method bodies
  // and the coverage invariant does not apply to them. SafepointElimination
  // skips them for the same reason.
  if (F.hasFnAttribute(jeandle::Attribute::LowerPhase))
    return PreservedAnalyses::all();

  auto &LI = AM.getResult<LoopAnalysis>(F);

  // SafepointElimination skips functions with irreducible cycles (LoopInfo
  // can't see those cycles, so neither it nor this check can reason about
  // their coverage). Match that: certifying only the natural loops here would
  // be a false assurance, so report the function as unverifiable instead.
  ReversePostOrderTraversal<const Function *> RPOT(&F);
  if (containsIrreducibleCFG<const BasicBlock *>(RPOT, LI)) {
    errs() << "SafepointCoverageVerifier: function '" << F.getName()
           << "' has an irreducible CFG; coverage not verified\n";
    return PreservedAnalyses::all();
  }

  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
  auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);

  bool Broken = false;
  for (Loop *L : LI.getLoopsInPreorder()) {
    if (isLoopCovered(*L, DT, SE)) {
      LLVM_DEBUG(dbgs() << "  covered: loop " << L->getHeader()->getName()
                        << " in " << F.getName() << "\n");
      continue;
    }
    Broken = true;
    errs() << "SafepointCoverageVerifier: loop with header '"
           << L->getHeader()->getName() << "' in function '" << F.getName()
           << "' has no dominating safepoint poll and no provable trip "
              "bound\n";
  }

  if (Broken && CoverageCheck == SafepointCoverageCheck::Fatal)
    report_fatal_error("Jeandle safepoint coverage verification failed");
  return PreservedAnalyses::all();
}
