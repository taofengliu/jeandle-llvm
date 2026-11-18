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
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Jeandle/Attributes.h"
#include "llvm/IR/Jeandle/JeandleUtils.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Jeandle/SafepointUtils.h"
#include "llvm/Transforms/Utils/LoopUtils.h"

using namespace llvm;

#define DEBUG_TYPE "safepoint-budget-strip-mining"

namespace {

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

} // namespace

PreservedAnalyses SafepointBudgetStripMining::run(Function &F,
                                                  FunctionAnalysisManager &AM) {
  uint64_t Budget = jeandle::getLoopStripMiningIter();
  if (!jeandle::isSafepointEliminationEnabled() || Budget < 2 ||
      !jeandle::isRootJavaMethodFunction(F))
    return PreservedAnalyses::all();

  jeandle::SafepointWorkBudget Work;
  bool Changed = false;
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
    }
    if (!Candidate || Work.exhausted())
      break;
    LLVM_DEBUG(dbgs() << "  bounded-counter batch: "
                      << Candidate->getHeader()->getName() << " in "
                      << F.getName() << "\n");
    wrapLoop(*Candidate, *Poll, Budget);
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
