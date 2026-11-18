//===- SafepointBudgetStripMining.cpp - Bounded poll batches -------------===//
//
// Copyright (c) 2026, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Jeandle/SafepointBudgetStripMining.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Jeandle/Attributes.h"
#include "llvm/IR/Jeandle/JeandleUtils.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Jeandle/SafepointUtils.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/LoopUtils.h"

using namespace llvm;

#define DEBUG_TYPE "safepoint-budget-strip-mining"

namespace {

constexpr StringLiteral CloneCostMetadata =
    "jeandle.safepoint.clone.instructions";

cl::opt<unsigned> MaxLoopCloneInstructions(
    "jeandle-safepoint-max-loop-clone-instructions", cl::Hidden, cl::init(128),
    cl::desc("Maximum source instructions copied for a polling batch."));
cl::opt<unsigned> MaxFunctionCloneInstructions(
    "jeandle-safepoint-max-function-clone-instructions", cl::Hidden,
    cl::init(512),
    cl::desc("Cumulative polling-batch clone budget per function."));

CallInst *findMovablePoll(Loop &L, LoopInfo &LI, DominatorTree &DT,
                          ScalarEvolution &SE, MemorySSA &MSSA,
                          jeandle::SafepointLoopAnalysis &A) {
  if (!L.isInnermost() || !L.isLoopSimplifyForm() || !L.isLCSSAForm(DT) ||
      !L.getLoopLatch() || jeandle::isMarkedStripMinedInner(L) ||
      jeandle::LoopSafepointFacts::get(L, SE).IsWithinBudget ||
      A.hasCoverage(L, true))
    return nullptr;

  // Moving a poll must not cross exception, convergence or token protocols.
  for (BasicBlock *BB : L.blocks()) {
    if (BB->isEHPad())
      return nullptr;
    for (Instruction &I : *BB) {
      if (I.getType()->isTokenTy())
        return nullptr;
      if (auto *CB = dyn_cast<CallBase>(&I))
        if (isa<InvokeInst>(CB) || CB->isConvergent() || CB->cannotDuplicate())
          return nullptr;
    }
  }
  SmallVector<BasicBlock *, 4> Exits;
  L.getUniqueExitBlocks(Exits);
  if (llvm::any_of(Exits, [](BasicBlock *BB) { return BB->isEHPad(); }))
    return nullptr;

  auto Requirements = jeandle::computeAncestorPollRequirements(L, A);
  jeandle::RequiredPolls Empty;
  CallInst *Poll = jeandle::findKeepOne(L, LI, DT, Empty);
  if (!Poll || !Poll->arg_empty() || Requirements.isRequired(*Poll, DT) ||
      !jeandle::canRelocateSafepointPoll(*Poll, L, MSSA))
    return nullptr;
  for (unsigned I = 0; I != Poll->getNumOperandBundles(); ++I)
    for (const Use &U : Poll->getOperandBundleAt(I).Inputs)
      if (U->getType()->isTokenTy())
        return nullptr;
  return Poll;
}

// The source exit edges and their values remain untouched. Only the continuing
// backedge is split: the independent counter either starts another source
// iteration, or leaves the bounded inner loop and polls before the next batch.
void wrapLoop(Loop &L, CallInst &Poll, uint64_t Budget) {
  BasicBlock *Header = L.getHeader();
  BasicBlock *Preheader = L.getLoopPreheader();
  BasicBlock *Latch = L.getLoopLatch();
  Function *F = Header->getParent();
  LLVMContext &C = F->getContext();
  BasicBlock *Outer =
      BasicBlock::Create(C, Header->getName() + ".batch", F, Header);
  BasicBlock *Check =
      BasicBlock::Create(C, Header->getName() + ".batch.check", F);
  BasicBlock *PollBB =
      BasicBlock::Create(C, Header->getName() + ".batch.poll", F);
  BranchInst *OuterBr = BranchInst::Create(Header, Outer);
  BranchInst *PollBr = BranchInst::Create(Outer, PollBB);

  DenseMap<Value *, Value *> BoundaryValues;
  auto AtBoundary = [&](Value *V) -> Value * {
    if (L.isLoopInvariant(V))
      return V;
    auto It = BoundaryValues.find(V);
    if (It != BoundaryValues.end())
      return It->second;
    auto *Phi = PHINode::Create(V->getType(), 1, V->getName() + ".batch.state",
                                PollBB->begin());
    Phi->addIncoming(V, Check);
    BoundaryValues[V] = Phi;
    return Phi;
  };

  SmallVector<PHINode *, 8> HeaderPhis;
  for (PHINode &Phi : Header->phis())
    HeaderPhis.push_back(&Phi);
  for (PHINode *Phi : HeaderPhis) {
    Value *Initial = Phi->getIncomingValueForBlock(Preheader);
    Value *Next = Phi->getIncomingValueForBlock(Latch);
    auto *OuterPhi = PHINode::Create(
        Phi->getType(), 2, Phi->getName() + ".batch", OuterBr->getIterator());
    OuterPhi->addIncoming(Initial, Preheader);
    OuterPhi->addIncoming(AtBoundary(Next), PollBB);
    Phi->setIncomingValue(Phi->getBasicBlockIndex(Preheader), OuterPhi);
    Phi->replaceIncomingBlockWith(Preheader, Outer);
    Phi->replaceIncomingBlockWith(Latch, Check);
  }

  auto *Tick =
      PHINode::Create(Type::getInt64Ty(C), 2, "batch.tick", Header->begin());
  Tick->addIncoming(ConstantInt::get(Tick->getType(), 0), Outer);
  IRBuilder<> B(Check);
  auto *NextTick = cast<BinaryOperator>(B.CreateAdd(
      Tick, ConstantInt::get(Tick->getType(), 1), "batch.tick.next"));
  NextTick->setHasNoUnsignedWrap();
  NextTick->setHasNoSignedWrap(Budget <= INT64_MAX);
  Tick->addIncoming(NextTick, Check);
  Value *Continue = B.CreateICmpULT(
      NextTick, ConstantInt::get(Tick->getType(), Budget), "batch.more");
  B.CreateCondBr(Continue, Header, PollBB);

  SmallVector<OperandBundleDef, 2> Bundles;
  for (unsigned I = 0; I != Poll.getNumOperandBundles(); ++I) {
    auto Bundle = Poll.getOperandBundleAt(I);
    SmallVector<Value *, 8> Inputs;
    for (const Use &U : Bundle.Inputs)
      Inputs.push_back(AtBoundary(U.get()));
    Bundles.emplace_back(Bundle.getTagName().str(), Inputs);
  }
  auto *Relocated = CallBase::Create(&Poll, Bundles, PollBr->getIterator());
  Relocated->addFnAttr(Attribute::get(C, jeandle::Attribute::StripMinedPoll));
  Poll.eraseFromParent();

  Preheader->getTerminator()->replaceSuccessorWith(Header, Outer);
  Latch->getTerminator()->replaceSuccessorWith(Header, Check);
}

// A clone plan is read-only until every structural, coverage and growth check
// succeeds. The original blocks will execute one complete polling iteration.
struct PollingIterationPlan {
  Loop *Source;
  unsigned Instructions;
};

std::optional<PollingIterationPlan>
planPollingIteration(Loop &L, jeandle::SafepointLoopAnalysis &A,
                     unsigned RemainingCloneBudget) {
  if (!L.isInnermost() || !L.isLoopSimplifyForm() || !L.isLCSSAForm(A.DT) ||
      !L.getLoopLatch() || !L.isSafeToClone() ||
      !isa<BranchInst>(L.getLoopLatch()->getTerminator()) ||
      jeandle::isMarkedStripMinedInner(L) || A.isExempt(L) ||
      A.hasCoverage(L, true) || !A.hasCoverage(L) || A.isEmptyLoopCandidate(L))
    return std::nullopt;

  if (llvm::count(successors(L.getLoopLatch()), L.getHeader()) != 1)
    return std::nullopt;
  unsigned Instructions = 0;
  bool HasPoll = false;
  for (BasicBlock *BB : L.blocks()) {
    if (BB->isEHPad() || BB->hasAddressTaken())
      return std::nullopt;
    for (Instruction &I : *BB) {
      if (!A.Work.spend())
        return std::nullopt;
      if (!I.isDebugOrPseudoInst() &&
          (++Instructions > MaxLoopCloneInstructions ||
           Instructions > RemainingCloneBudget))
        return std::nullopt;
      if (I.getType()->isTokenTy() ||
          (I.isTerminator() && !isa<BranchInst, SwitchInst>(&I)))
        return std::nullopt;
      for (Value *Operand : I.operands())
        if (Operand->getType()->isTokenTy())
          return std::nullopt;
      auto *CB = dyn_cast<CallBase>(&I);
      if (!CB)
        continue;
      if (!isa<CallInst>(CB) || CB->isConvergent() || CB->cannotDuplicate() ||
          CB->hasFnAttr(Attribute::ReturnsTwice) ||
          CB->hasFnAttr(jeandle::Attribute::StatepointID))
        return std::nullopt;
      if (jeandle::isSafepointPoll(I)) {
        if (!CB->arg_empty() || !CB->use_empty())
          return std::nullopt;
        HasPoll = true;
      }
    }
  }
  SmallVector<BasicBlock *, 4> Exits;
  L.getUniqueExitBlocks(Exits);
  if (!HasPoll ||
      llvm::any_of(Exits, [](BasicBlock *BB) { return BB->isEHPad(); }))
    return std::nullopt;

  // A fast natural exit can skip the polling iteration completely. Every
  // ancestor must therefore have coverage independent of all source polls.
  auto Requirements = jeandle::computeAncestorPollRequirements(L, A);
  if (!Requirements.Complete || !Requirements.Polls.empty())
    return std::nullopt;
  return PollingIterationPlan{&L, Instructions};
}

void clonePollingIteration(const PollingIterationPlan &Plan, uint64_t Budget,
                           LoopInfo &LI, DominatorTree &DT) {
  Loop &L = *Plan.Source;
  BasicBlock *Header = L.getHeader();
  BasicBlock *Latch = L.getLoopLatch();
  BasicBlock *Preheader = L.getLoopPreheader();
  Function *F = Header->getParent();
  LLVMContext &C = F->getContext();

  struct CarriedState {
    PHINode *Phi;
    Value *Initial;
    Value *Next;
  };
  SmallVector<CarriedState, 8> State;
  for (PHINode &Phi : Header->phis())
    State.push_back({&Phi, Phi.getIncomingValueForBlock(Preheader),
                     Phi.getIncomingValueForBlock(Latch)});
  SmallVector<BasicBlock *, 8> SourceBlocks(L.blocks());
  SmallVector<MDNode *, 4> NoAliasScopes;
  identifyNoAliasScopesToClone(SourceBlocks, NoAliasScopes);

  // The cloning utility copies its entire preheader. Hoisted loads, stores
  // and calls must stay outside the new outer loop and execute only once.
  // Keep LI/DT valid through this split and the cloning utility's updates.
  BasicBlock *Outer = SplitBlock(Preheader, Preheader->getTerminator(), &DT,
                                 &LI, nullptr, "batch.outer");
  ValueToValueMapTy VMap;
  SmallVector<BasicBlock *, 8> ClonedBlocks;
  cloneLoopWithPreheader(Header, Outer, &L, VMap, ".fast", &LI, &DT,
                         ClonedBlocks);
  remapInstructionsInBlocks(ClonedBlocks, VMap);
  cloneAndAdaptNoAliasScopes(NoAliasScopes, ClonedBlocks, C, "fast");
  auto Mapped = [&](Value *V) -> Value * {
    auto It = VMap.find(V);
    return It == VMap.end() ? V : static_cast<Value *>(It->second);
  };
  auto *FastHeader = cast<BasicBlock>(Mapped(Header));
  auto *FastLatch = cast<BasicBlock>(Mapped(Latch));
  auto *FastPreheader = cast<BasicBlock>(Mapped(Outer));

  // From here on, LI/DT are deliberately not queried. Rebuild immediately
  // after the batch of CFG edits, before forming LCSSA or finding candidates.
  for (BasicBlock *BB : SourceBlocks) {
    SmallPtrSet<BasicBlock *, 4> Seen;
    for (BasicBlock *Exit : successors(BB)) {
      if (VMap.count(Exit) || !Seen.insert(Exit).second)
        continue;
      for (PHINode &Phi : Exit->phis()) {
        unsigned IncomingCount = Phi.getNumIncomingValues();
        for (unsigned I = 0; I != IncomingCount; ++I)
          if (Phi.getIncomingBlock(I) == BB)
            Phi.addIncoming(Mapped(Phi.getIncomingValue(I)),
                            cast<BasicBlock>(Mapped(BB)));
      }
    }
  }

  BasicBlock *Check = BasicBlock::Create(C, "batch.check", F);
  for (const CarriedState &S : State) {
    auto *OuterPhi =
        PHINode::Create(S.Phi->getType(), 2, S.Phi->getName() + ".batch",
                        Outer->getTerminator()->getIterator());
    OuterPhi->addIncoming(S.Initial, Preheader);
    OuterPhi->addIncoming(S.Next, Latch);
    auto *FastPhi = cast<PHINode>(Mapped(S.Phi));
    FastPhi->setIncomingValue(FastPhi->getBasicBlockIndex(FastPreheader),
                              OuterPhi);
    FastPhi->replaceIncomingBlockWith(FastLatch, Check);
    // Capture by incoming-edge role, including self and mutually cyclic PHIs.
    // This is the state after the last fast iteration, at the beginning of
    // the complete original iteration, not that iteration's next state.
    S.Phi->removeIncomingValue(S.Phi->getBasicBlockIndex(Latch), false);
    S.Phi->setIncomingBlock(0, Check);
    S.Phi->setIncomingValue(0, Mapped(S.Next));
  }

  auto *Tick = PHINode::Create(Type::getInt64Ty(C), 2, "batch.tick",
                               FastHeader->begin());
  Tick->addIncoming(ConstantInt::get(Tick->getType(), 0), FastPreheader);
  IRBuilder<> B(Check);
  auto *NextTick = cast<BinaryOperator>(B.CreateAdd(
      Tick, ConstantInt::get(Tick->getType(), 1), "batch.tick.next"));
  NextTick->setHasNoUnsignedWrap();
  NextTick->setHasNoSignedWrap(Budget - 1 <= INT64_MAX);
  Tick->addIncoming(NextTick, Check);
  Value *More = B.CreateICmpULT(
      NextTick, ConstantInt::get(Tick->getType(), Budget - 1), "batch.more");
  B.CreateCondBr(More, FastHeader, Header);

  for (BasicBlock *BB : ClonedBlocks)
    for (Instruction &I : make_early_inc_range(*BB))
      if (jeandle::isSafepointPoll(I))
        I.eraseFromParent();
  Outer->getTerminator()->replaceSuccessorWith(Header, FastPreheader);
  FastLatch->getTerminator()->replaceSuccessorWith(FastHeader, Check);
  Latch->getTerminator()->replaceSuccessorWith(Header, Outer);
}

} // namespace

PreservedAnalyses SafepointBudgetStripMining::run(Function &F,
                                                  FunctionAnalysisManager &AM) {
  uint64_t Budget = jeandle::getLoopStripMiningIter();
  if (!jeandle::isSafepointEliminationEnabled() || Budget < 2 ||
      !jeandle::isRootJavaMethodFunction(F))
    return PreservedAnalyses::all();

  jeandle::SafepointWorkBudget Work;
  bool Changed = false;
  unsigned ClonedInstructions = 0;
  // Keep growth accounting across repeated pass invocations. This metadata
  // limits code size; it never substitutes for a fast-loop bound proof.
  if (MDNode *Cost = F.getMetadata(CloneCostMetadata)) {
    auto *Count = Cost->getNumOperands() == 1
                      ? mdconst::dyn_extract<ConstantInt>(Cost->getOperand(0))
                      : nullptr;
    ClonedInstructions =
        Count ? std::min<uint64_t>(MaxFunctionCloneInstructions,
                                   Count->getValue().getLimitedValue())
              : MaxFunctionCloneInstructions.getValue();
  }
  // Candidate identities survive CFG rebuilding. No SCEV or MemorySSA query is
  // made after a mutation; even sibling loops may share exit or memory state.
  SmallPtrSet<BasicBlock *, 16> Examined;
  while (!Work.exhausted()) {
    if (!Work.spend(F.getInstructionCount()))
      break;
    auto &LI = AM.getResult<LoopAnalysis>(F);
    ReversePostOrderTraversal<const Function *> RPOT(&F);
    if (containsIrreducibleCFG<const BasicBlock *>(RPOT, LI))
      break;
    auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
    auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
    auto &MSSA = AM.getResult<MemorySSAAnalysis>(F).getMSSA();
    jeandle::SafepointLoopAnalysis A(LI, DT, SE, Work);
    Loop *Candidate = nullptr;
    CallInst *Poll = nullptr;
    std::optional<PollingIterationPlan> ClonePlan;
    for (Loop *L : LI.getLoopsInPreorder()) {
      if (!Examined.insert(L->getHeader()).second)
        continue;
      if (!A.checkLoop(*L))
        break;
      if (DeferEmptyLoopDeletion) {
        bool Deferred = false;
        for (Loop *Ancestor = L; Ancestor; Ancestor = Ancestor->getParentLoop())
          Deferred |= A.isEmptyLoopCandidate(*Ancestor);
        if (Deferred)
          continue;
      }
      Poll = findMovablePoll(*L, LI, DT, SE, MSSA, A);
      if (Poll) {
        Candidate = L;
        break;
      }
      ClonePlan = planPollingIteration(
          *L, A, MaxFunctionCloneInstructions - ClonedInstructions);
      if (ClonePlan) {
        Candidate = L;
        break;
      }
    }
    if (!Candidate || Work.exhausted())
      break;
    LLVM_DEBUG(dbgs() << "  bounded-counter batch: "
                      << Candidate->getHeader()->getName() << " in "
                      << F.getName() << "\n");
    if (Poll) {
      wrapLoop(*Candidate, *Poll, Budget);
    } else {
      clonePollingIteration(*ClonePlan, Budget, LI, DT);
      ClonedInstructions += ClonePlan->Instructions;
      F.setMetadata(
          CloneCostMetadata,
          MDNode::get(F.getContext(), ConstantAsMetadata::get(ConstantInt::get(
                                          Type::getInt64Ty(F.getContext()),
                                          ClonedInstructions))));
    }
    Changed = true;
    AM.invalidate(F, PreservedAnalyses::none());
    auto &NewDT = AM.getResult<DominatorTreeAnalysis>(F);
    auto &NewLI = AM.getResult<LoopAnalysis>(F);
    for (Loop *Top : NewLI)
      formLCSSARecursively(*Top, NewDT, &NewLI, nullptr);
    // LCSSA may have inserted PHIs and changed operand-bundle uses.
    AM.invalidate(F, PreservedAnalyses::none());
  }
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
