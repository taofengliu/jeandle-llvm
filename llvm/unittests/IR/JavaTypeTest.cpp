//===- JavaTypeTest.cpp - Java base type queries
//---------------------------===//
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/Jeandle/JavaType.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Jeandle/VMCallback.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::jeandle;

namespace {

class JavaBaseTypeTest : public testing::Test {
protected:
  LLVMContext Context;
  Module M{"java-type-test", Context};
  IRBuilder<> B{Context};
  VMCallbacks Saved{};
  Function *F = nullptr;
  BasicBlock *Entry = nullptr;
  BasicBlock *Loop = nullptr;
  static inline unsigned CommonSuperQueries = 0;
  static inline unsigned InterfaceQueries = 0;

  void SetUp() override {
    CommonSuperQueries = 0;
    InterfaceQueries = 0;
    if (const VMCallbacks *CB = getVMCallbacks())
      Saved = *CB;
    VMCallbacks CB{};
    CB.IsInterface = [](uintptr_t) { return false; };
    CB.IsEffectivelyFinal = [](uintptr_t) { return false; };
    CB.GetSecondarySupers = [](uintptr_t) {
      ++InterfaceQueries;
      return std::vector<uintptr_t>{};
    };
    CB.IsSubtype = [](uintptr_t A, uintptr_t B) { return A == B || B == 5; };
    CB.GetCommonSuperKlass = [](uintptr_t A, uintptr_t B) {
      ++CommonSuperQueries;
      return A == B ? A : uintptr_t(5);
    };
    registerVMCallbacks(CB);
    auto *Ptr = PointerType::get(Context, 1);
    F = Function::Create(
        FunctionType::get(Ptr, {Ptr, Ptr, B.getInt1Ty(), Ptr}, false),
        GlobalValue::ExternalLinkage, "test", M);
    Entry = BasicBlock::Create(Context, "entry", F);
    Loop = BasicBlock::Create(Context, "loop", F);
    B.SetInsertPoint(Entry);
    B.CreateBr(Loop);
    B.SetInsertPoint(Loop);
  }

  void TearDown() override { registerVMCallbacks(Saved); }

  Argument *seed(unsigned Index, unsigned Klass, bool Exact = false) {
    F->addParamAttr(
        Index, Attribute::get(Context, "java-klass", std::to_string(Klass)));
    if (Exact)
      F->addParamAttr(Index, Attribute::get(Context, "java-klass-exact"));
    return F->getArg(Index);
  }

  JavaType query(Value *V) { return getJavaType(V, nullptr, nullptr, nullptr); }

  JavaType queryAt(Value *V, Instruction *At, DominatorTree &DT) {
    return getJavaType(
        V, &DT, At, [](Value *, BasicBlock *, BasicBlock *) { return false; });
  }

  Value *typedSelectChain(unsigned Depth) {
    Value *V = seed(0, 9, true);
    Value *Other = seed(1, 22, true);
    for (unsigned I = 0; I != Depth; ++I)
      V = B.CreateSelect(F->getArg(2), V, Other);
    return V;
  }

  void checkGuardedIncomingReuse(bool UnknownFirst) {
    constexpr unsigned Depth = 2048;
    constexpr unsigned Arms = 64;
    Value *Known = seed(0, 9);
    Value *Shared = F->getArg(3);
    if (UnknownFirst) {
      for (unsigned I = 0; I != Depth; ++I)
        Shared = B.CreateSelect(F->getArg(2), Shared, Known);
    } else {
      Shared = typedSelectChain(Depth);
    }
    Value *Other = UnknownFirst ? Known : F->getArg(3);
    auto *Fallback = BasicBlock::Create(Context, "fallback", F);
    auto *Merge = BasicBlock::Create(Context, "merge", F);
    auto *Root = PHINode::Create(F->getReturnType(), Arms, "root", Merge);
    auto *Dispatch = B.CreateSwitch(
        B.CreatePtrToInt(F->getArg(1), B.getInt64Ty()), Fallback, Arms);
    auto *RawPtr = PointerType::get(Context, 0);
    FunctionCallee Check = M.getOrInsertFunction(
        "jeandle.check_instanceof", B.getInt1Ty(), RawPtr, F->getReturnType());
    auto *Klass = ConstantExpr::getIntToPtr(B.getInt64(9), RawPtr);
    for (unsigned I = 0; I != Arms; ++I) {
      auto *Guard = BasicBlock::Create(Context, "guard", F);
      auto *Accepted = BasicBlock::Create(Context, "accepted", F);
      Dispatch->addCase(B.getInt64(I), Guard);
      B.SetInsertPoint(Guard);
      auto *Incoming = B.CreateSelect(F->getArg(2), Shared, Other);
      auto *Condition = B.CreateCall(Check, {Klass, Incoming});
      B.CreateCondBr(Condition, Accepted, Fallback);
      B.SetInsertPoint(Accepted);
      B.CreateBr(Merge);
      Root->addIncoming(Incoming, Accepted);
    }
    B.SetInsertPoint(Fallback);
    B.CreateRet(
        ConstantPointerNull::get(cast<PointerType>(F->getReturnType())));
    B.SetInsertPoint(Merge);
    auto *Return = B.CreateRet(Root);
    ASSERT_FALSE(verifyFunction(*F));
    DominatorTree DT(*F);
    JavaType T = queryAt(Root, Return, DT);
    EXPECT_EQ(T.Klass, 9U);
    EXPECT_FALSE(T.Exact);
  }
};

TEST_F(JavaBaseTypeTest, SharedDagAboveSelfCycle) {
  auto *P = B.CreatePHI(F->getReturnType(), 2);
  P->addIncoming(seed(0, 9, true), Entry);
  P->addIncoming(P, Loop);
  BasicBlock *Previous = Loop;
  // Each merge has two CFG edges carrying the same value. The old recursion
  // walked both paths again because the bottom self-cycle disabled caching.
  for (unsigned I = 0; I != 40; ++I) {
    auto *Next = BasicBlock::Create(Context, "merge", F);
    B.SetInsertPoint(Previous);
    B.CreateCondBr(F->getArg(2), Next, Next);
    B.SetInsertPoint(Next);
    auto *Merge = B.CreatePHI(F->getReturnType(), 2);
    Merge->addIncoming(P, Previous);
    Merge->addIncoming(P, Previous);
    P = Merge;
    Previous = Next;
  }
  B.CreateBr(Loop);
  // Close the seed PHI using the real back-edge predecessor.
  cast<PHINode>(&Loop->front())->setIncomingBlock(1, Previous);
  JavaType T = query(P);
  EXPECT_EQ(T.Klass, 9U);
  EXPECT_TRUE(T.Exact);
}

TEST_F(JavaBaseTypeTest, MutualCycleJoinsAllSeeds) {
  auto *A = B.CreatePHI(F->getReturnType(), 2);
  auto *C = B.CreatePHI(F->getReturnType(), 2);
  A->addIncoming(seed(0, 9, true), Entry);
  A->addIncoming(C, Loop);
  C->addIncoming(seed(1, 22, true), Entry);
  C->addIncoming(A, Loop);
  EXPECT_EQ(query(A).Klass, 5U);
  EXPECT_EQ(query(C).Klass, 5U);
  EXPECT_FALSE(query(A).Exact);
}

TEST_F(JavaBaseTypeTest, UnknownArmMustNotBeDropped) {
  auto *P = B.CreatePHI(F->getReturnType(), 3);
  P->addIncoming(seed(0, 9, true), Entry);
  P->addIncoming(P, Loop);
  P->addIncoming(F->getArg(1), Loop);
  EXPECT_TRUE(query(P).isUnknown());
  for (Value *Unknown :
       {static_cast<Value *>(
            ConstantPointerNull::get(cast<PointerType>(F->getReturnType()))),
        static_cast<Value *>(PoisonValue::get(F->getReturnType())),
        static_cast<Value *>(UndefValue::get(F->getReturnType()))}) {
    P->setIncomingValue(2, Unknown);
    EXPECT_TRUE(query(P).isUnknown());
  }
}

TEST_F(JavaBaseTypeTest, UnknownSelectArmShortCircuits) {
  Value *Known = typedSelectChain(2048);
  auto *Root = B.CreateSelect(F->getArg(2), F->getArg(3), Known);
  B.CreateRet(Root);
  ASSERT_FALSE(verifyFunction(*F));
  EXPECT_TRUE(query(Root).isUnknown());
  EXPECT_EQ(CommonSuperQueries, 0U);
  EXPECT_EQ(InterfaceQueries, 0U);
}

TEST_F(JavaBaseTypeTest, NestedUnknownArmShortCircuits) {
  Value *Known = typedSelectChain(2048);
  auto *Unknown = B.CreateSelect(F->getArg(2), F->getArg(3), F->getArg(0));
  auto *Root = B.CreateSelect(F->getArg(2), Unknown, Known);
  B.CreateRet(Root);
  ASSERT_FALSE(verifyFunction(*F));
  EXPECT_TRUE(query(Root).isUnknown());
  EXPECT_EQ(CommonSuperQueries, 0U);
  EXPECT_EQ(InterfaceQueries, 0U);
}

TEST_F(JavaBaseTypeTest, UnknownPhiArmShortCircuits) {
  auto *Root = B.CreatePHI(F->getReturnType(), 2);
  Root->addIncoming(F->getArg(3), Entry);
  Root->addIncoming(typedSelectChain(2048), Loop);
  auto *Exit = BasicBlock::Create(Context, "exit", F);
  B.CreateCondBr(F->getArg(2), Loop, Exit);
  B.SetInsertPoint(Exit);
  B.CreateRet(Root);
  ASSERT_FALSE(verifyFunction(*F));
  EXPECT_TRUE(query(Root).isUnknown());
  EXPECT_EQ(CommonSuperQueries, 0U);
  EXPECT_EQ(InterfaceQueries, 0U);
}

TEST_F(JavaBaseTypeTest, ContextQueryReusesCompletedCycle) {
  auto *A = B.CreatePHI(F->getReturnType(), 2);
  auto *C = B.CreatePHI(F->getReturnType(), 2);
  A->addIncoming(seed(0, 9, true), Entry);
  A->addIncoming(C, Loop);
  C->addIncoming(seed(1, 22, true), Entry);
  C->addIncoming(A, Loop);
  auto *Fork = BasicBlock::Create(Context, "fork", F);
  auto *Left = BasicBlock::Create(Context, "left", F);
  auto *Right = BasicBlock::Create(Context, "right", F);
  auto *Merge = BasicBlock::Create(Context, "merge", F);
  B.CreateCondBr(F->getArg(2), Loop, Fork);
  B.SetInsertPoint(Fork);
  B.CreateCondBr(F->getArg(2), Left, Right);
  B.SetInsertPoint(Left);
  auto *LeftValue = B.CreateSelect(F->getArg(2), A, F->getArg(0));
  B.CreateBr(Merge);
  B.SetInsertPoint(Right);
  auto *RightValue = B.CreateSelect(F->getArg(2), C, F->getArg(1));
  B.CreateBr(Merge);
  B.SetInsertPoint(Merge);
  auto *Root = B.CreatePHI(F->getReturnType(), 2);
  Root->addIncoming(LeftValue, Left);
  Root->addIncoming(RightValue, Right);
  auto *Return = B.CreateRet(Root);
  ASSERT_FALSE(verifyFunction(*F));
  DominatorTree DT(*F);
  JavaType T = queryAt(Root, Return, DT);
  EXPECT_EQ(T.Klass, 5U);
  EXPECT_FALSE(T.Exact);
}

TEST_F(JavaBaseTypeTest, ContextGuardDoesNotPolluteBaseCache) {
  auto *A = B.CreatePHI(F->getReturnType(), 2);
  auto *C = B.CreatePHI(F->getReturnType(), 2);
  A->addIncoming(seed(0, 9, true), Entry);
  A->addIncoming(C, Loop);
  C->addIncoming(seed(1, 22, true), Entry);
  C->addIncoming(A, Loop);
  auto *Fork = BasicBlock::Create(Context, "fork", F);
  auto *Left = BasicBlock::Create(Context, "left", F);
  auto *Right = BasicBlock::Create(Context, "right", F);
  auto *Merge = BasicBlock::Create(Context, "merge", F);
  B.CreateCondBr(F->getArg(2), Loop, Fork);
  B.SetInsertPoint(Fork);
  auto *Shared = B.CreateSelect(F->getArg(2), A, C);
  auto *RawPtr = PointerType::get(Context, 0);
  FunctionCallee Check = M.getOrInsertFunction(
      "jeandle.check_instanceof", B.getInt1Ty(), RawPtr, F->getReturnType());
  auto *Klass = ConstantExpr::getIntToPtr(B.getInt64(9), RawPtr);
  auto *Condition = B.CreateCall(Check, {Klass, Shared});
  B.CreateCondBr(Condition, Left, Right);
  B.SetInsertPoint(Left);
  auto *LeftEnd = B.CreateBr(Merge);
  B.SetInsertPoint(Right);
  B.CreateBr(Merge);
  B.SetInsertPoint(Merge);
  auto *Root = B.CreatePHI(F->getReturnType(), 2);
  Root->addIncoming(Shared, Left);
  Root->addIncoming(Shared, Right);
  auto *Return = B.CreateRet(Root);
  ASSERT_FALSE(verifyFunction(*F));
  DominatorTree DT(*F);
  EXPECT_EQ(queryAt(Shared, LeftEnd, DT).Klass, 9U);
  JavaType T = queryAt(Root, Return, DT);
  EXPECT_EQ(T.Klass, 5U);
  EXPECT_FALSE(T.Exact);
}

TEST_F(JavaBaseTypeTest, GuardedIncomingValuesReuseSharedUnknownChain) {
  checkGuardedIncomingReuse(true);
}

TEST_F(JavaBaseTypeTest, GuardedIncomingValuesReuseSharedTypedChain) {
  checkGuardedIncomingReuse(false);
}

TEST_F(JavaBaseTypeTest, NestedPhisCompleteSharedInputsBeforeShortCircuit) {
  constexpr unsigned Depth = 2048;
  constexpr unsigned Arms = 64;
  Value *Shared = typedSelectChain(Depth);
  auto *Fallback = BasicBlock::Create(Context, "fallback", F);
  auto *Merge = BasicBlock::Create(Context, "merge", F);
  auto *Root = PHINode::Create(F->getReturnType(), Arms, "root", Merge);
  auto *Dispatch = B.CreateSwitch(
      B.CreatePtrToInt(F->getArg(1), B.getInt64Ty()), Fallback, Arms);
  auto *RawPtr = PointerType::get(Context, 0);
  FunctionCallee Check = M.getOrInsertFunction(
      "jeandle.check_instanceof", B.getInt1Ty(), RawPtr, F->getReturnType());
  auto *Klass = ConstantExpr::getIntToPtr(B.getInt64(9), RawPtr);
  for (unsigned I = 0; I != Arms; ++I) {
    auto *Fork = BasicBlock::Create(Context, "fork", F);
    auto *Typed = BasicBlock::Create(Context, "typed", F);
    auto *Unknown = BasicBlock::Create(Context, "unknown", F);
    auto *Other = BasicBlock::Create(Context, "other", F);
    auto *InnerMerge = BasicBlock::Create(Context, "inner_merge", F);
    auto *Accepted = BasicBlock::Create(Context, "accepted", F);
    Dispatch->addCase(B.getInt64(I), Fork);
    B.SetInsertPoint(Fork);
    auto *Choice = B.CreateSwitch(
        B.CreatePtrToInt(F->getArg(3), B.getInt64Ty()), Other, 2);
    Choice->addCase(B.getInt64(0), Typed);
    Choice->addCase(B.getInt64(1), Unknown);
    for (BasicBlock *BB : {Typed, Unknown, Other}) {
      B.SetInsertPoint(BB);
      B.CreateBr(InnerMerge);
    }
    B.SetInsertPoint(InnerMerge);
    auto *Inner = B.CreatePHI(F->getReturnType(), 3);
    Inner->addIncoming(Shared, Typed);
    Inner->addIncoming(F->getArg(3), Unknown);
    Inner->addIncoming(F->getArg(1), Other);
    auto *Incoming = B.CreateSelect(F->getArg(2), Inner, F->getArg(0));
    B.CreateCondBr(B.CreateCall(Check, {Klass, Incoming}), Accepted, Fallback);
    B.SetInsertPoint(Accepted);
    B.CreateBr(Merge);
    Root->addIncoming(Incoming, Accepted);
  }
  B.SetInsertPoint(Fallback);
  B.CreateRet(ConstantPointerNull::get(cast<PointerType>(F->getReturnType())));
  B.SetInsertPoint(Merge);
  auto *Return = B.CreateRet(Root);
  ASSERT_FALSE(verifyFunction(*F));
  DominatorTree DT(*F);
  JavaType T = queryAt(Root, Return, DT);
  EXPECT_EQ(T.Klass, 9U);
  EXPECT_FALSE(T.Exact);
  EXPECT_LE(CommonSuperQueries, Depth + 4 * Arms);
}

TEST_F(JavaBaseTypeTest, SelectCycleShortCircuitsTypedSibling) {
  B.SetInsertPoint(Entry->getTerminator());
  Value *Other = typedSelectChain(2048);
  B.SetInsertPoint(Loop);
  auto *P = B.CreatePHI(F->getReturnType(), 2);
  auto *Selected = B.CreateSelect(F->getArg(2), P, Other);
  P->addIncoming(F->getArg(0), Entry);
  P->addIncoming(Selected, Loop);
  auto *Exit = BasicBlock::Create(Context, "exit", F);
  B.CreateCondBr(F->getArg(2), Loop, Exit);
  B.SetInsertPoint(Exit);
  B.CreateRet(P);
  ASSERT_FALSE(verifyFunction(*F));
  EXPECT_TRUE(query(P).isUnknown());
  EXPECT_EQ(CommonSuperQueries, 0U);
}

TEST_F(JavaBaseTypeTest, SelectCycleThroughPhiShortCircuitsTypedSibling) {
  B.SetInsertPoint(Entry->getTerminator());
  Value *Other = typedSelectChain(2048);
  B.SetInsertPoint(Loop);
  auto *P = B.CreatePHI(F->getReturnType(), 2);
  auto *Q = B.CreatePHI(F->getReturnType(), 2);
  auto *Selected = B.CreateSelect(F->getArg(2), Q, Other);
  P->addIncoming(F->getArg(0), Entry);
  P->addIncoming(Selected, Loop);
  Q->addIncoming(F->getArg(0), Entry);
  Q->addIncoming(P, Loop);
  auto *Exit = BasicBlock::Create(Context, "exit", F);
  B.CreateCondBr(F->getArg(2), Loop, Exit);
  B.SetInsertPoint(Exit);
  B.CreateRet(P);
  ASSERT_FALSE(verifyFunction(*F));
  EXPECT_TRUE(query(P).isUnknown());
  EXPECT_EQ(CommonSuperQueries, 0U);
}

TEST_F(JavaBaseTypeTest, NonPhiAncestorOutsideCyclePreservesType) {
  auto *A = B.CreatePHI(F->getReturnType(), 2);
  auto *C = B.CreatePHI(F->getReturnType(), 2);
  A->addIncoming(seed(0, 9, true), Entry);
  A->addIncoming(C, Loop);
  C->addIncoming(seed(1, 22, true), Entry);
  C->addIncoming(A, Loop);
  auto *Exit = BasicBlock::Create(Context, "exit", F);
  B.CreateCondBr(F->getArg(2), Loop, Exit);
  B.SetInsertPoint(Exit);
  auto *Root = B.CreateSelect(F->getArg(2), A, C);
  B.CreateRet(Root);
  ASSERT_FALSE(verifyFunction(*F));
  JavaType T = query(Root);
  EXPECT_EQ(T.Klass, 5U);
  EXPECT_FALSE(T.Exact);
}

TEST_F(JavaBaseTypeTest, CrossBranchSelectCycleShortCircuitsTypedSibling) {
  B.SetInsertPoint(Entry->getTerminator());
  Value *Other = typedSelectChain(2048);
  B.SetInsertPoint(Loop);
  auto *A = B.CreatePHI(F->getReturnType(), 3);
  auto *C = B.CreatePHI(F->getReturnType(), 3);
  auto *Left = BasicBlock::Create(Context, "left", F);
  auto *Right = BasicBlock::Create(Context, "right", F);
  auto *Exit = BasicBlock::Create(Context, "exit", F);
  B.CreateCondBr(F->getArg(2), Left, Right);
  B.SetInsertPoint(Left);
  B.CreateBr(Loop);
  B.SetInsertPoint(Right);
  auto *Selected = B.CreateSelect(F->getArg(2), C, Other);
  B.CreateCondBr(F->getArg(2), Loop, Exit);
  A->addIncoming(F->getArg(0), Entry);
  A->addIncoming(C, Left);
  A->addIncoming(Selected, Right);
  C->addIncoming(F->getArg(0), Entry);
  C->addIncoming(A, Left);
  C->addIncoming(A, Right);
  B.SetInsertPoint(Exit);
  B.CreateRet(A);
  ASSERT_FALSE(verifyFunction(*F));
  EXPECT_TRUE(query(A).isUnknown());
  EXPECT_EQ(CommonSuperQueries, 0U);
}

TEST_F(JavaBaseTypeTest, CrossBranchPurePhiCyclePreservesType) {
  auto *A = B.CreatePHI(F->getReturnType(), 3);
  auto *C = B.CreatePHI(F->getReturnType(), 3);
  auto *D = B.CreatePHI(F->getReturnType(), 3);
  auto *Left = BasicBlock::Create(Context, "left", F);
  auto *Right = BasicBlock::Create(Context, "right", F);
  auto *Exit = BasicBlock::Create(Context, "exit", F);
  B.CreateCondBr(F->getArg(2), Left, Right);
  B.SetInsertPoint(Left);
  B.CreateBr(Loop);
  B.SetInsertPoint(Right);
  B.CreateCondBr(F->getArg(2), Loop, Exit);
  A->addIncoming(seed(0, 9, true), Entry);
  A->addIncoming(C, Left);
  A->addIncoming(D, Right);
  C->addIncoming(seed(1, 22, true), Entry);
  C->addIncoming(A, Left);
  C->addIncoming(A, Right);
  D->addIncoming(F->getArg(0), Entry);
  D->addIncoming(C, Left);
  D->addIncoming(C, Right);
  B.SetInsertPoint(Exit);
  B.CreateRet(A);
  ASSERT_FALSE(verifyFunction(*F));
  JavaType T = query(A);
  EXPECT_EQ(T.Klass, 5U);
  EXPECT_FALSE(T.Exact);
}

TEST_F(JavaBaseTypeTest, CycleShortCircuitPreservesIndependentPhiCache) {
  auto *A = B.CreatePHI(F->getReturnType(), 2);
  auto *C = B.CreatePHI(F->getReturnType(), 2);
  auto *P = B.CreatePHI(F->getReturnType(), 2);
  A->addIncoming(seed(0, 9, true), Entry);
  A->addIncoming(C, Loop);
  C->addIncoming(seed(1, 22, true), Entry);
  C->addIncoming(A, Loop);
  P->addIncoming(F->getArg(0), Entry);
  auto *Selected = B.CreateSelect(F->getArg(2), P, F->getArg(0));
  P->addIncoming(Selected, Loop);
  auto *Fork = BasicBlock::Create(Context, "fork", F);
  auto *Guard = BasicBlock::Create(Context, "guard", F);
  auto *Accepted = BasicBlock::Create(Context, "accepted", F);
  auto *Right = BasicBlock::Create(Context, "right", F);
  auto *Fallback = BasicBlock::Create(Context, "fallback", F);
  auto *Merge = BasicBlock::Create(Context, "merge", F);
  B.CreateCondBr(F->getArg(2), Loop, Fork);
  B.SetInsertPoint(Fork);
  B.CreateCondBr(F->getArg(2), Guard, Right);
  B.SetInsertPoint(Guard);
  // Completed A/C types must survive the unsupported cycle in the other arm.
  auto *LeftValue = B.CreateSelect(F->getArg(2), A, P);
  auto *RawPtr = PointerType::get(Context, 0);
  FunctionCallee Check = M.getOrInsertFunction(
      "jeandle.check_instanceof", B.getInt1Ty(), RawPtr, F->getReturnType());
  auto *Klass = ConstantExpr::getIntToPtr(B.getInt64(9), RawPtr);
  B.CreateCondBr(B.CreateCall(Check, {Klass, LeftValue}), Accepted, Fallback);
  B.SetInsertPoint(Accepted);
  B.CreateBr(Merge);
  B.SetInsertPoint(Right);
  auto *RightValue = B.CreateSelect(F->getArg(2), C, F->getArg(1));
  B.CreateBr(Merge);
  B.SetInsertPoint(Fallback);
  B.CreateRet(ConstantPointerNull::get(cast<PointerType>(F->getReturnType())));
  B.SetInsertPoint(Merge);
  auto *Root = B.CreatePHI(F->getReturnType(), 2);
  Root->addIncoming(LeftValue, Accepted);
  Root->addIncoming(RightValue, Right);
  auto *Return = B.CreateRet(Root);
  ASSERT_FALSE(verifyFunction(*F));
  DominatorTree DT(*F);
  JavaType T = queryAt(Root, Return, DT);
  EXPECT_EQ(T.Klass, 5U);
  EXPECT_FALSE(T.Exact);
}

TEST_F(JavaBaseTypeTest, DeepTypedChainDoesNotUseTheCallStack) {
  constexpr unsigned Depth = 20000;
  Value *Root = typedSelectChain(Depth);
  B.CreateRet(Root);
  ASSERT_FALSE(verifyFunction(*F));
  JavaType T = query(Root);
  EXPECT_EQ(T.Klass, 5U);
  EXPECT_FALSE(T.Exact);
  EXPECT_LE(CommonSuperQueries, Depth);
}

TEST_F(JavaBaseTypeTest, ShortCircuitDoesNotCacheUnfinishedSibling) {
  auto *A = B.CreatePHI(F->getReturnType(), 2);
  auto *C = B.CreatePHI(F->getReturnType(), 2);
  A->addIncoming(seed(0, 9, true), Entry);
  A->addIncoming(C, Loop);
  C->addIncoming(seed(1, 22, true), Entry);
  C->addIncoming(A, Loop);
  auto *Fork = BasicBlock::Create(Context, "fork", F);
  auto *Guard = BasicBlock::Create(Context, "guard", F);
  auto *Accepted = BasicBlock::Create(Context, "accepted", F);
  auto *Right = BasicBlock::Create(Context, "right", F);
  auto *Fallback = BasicBlock::Create(Context, "fallback", F);
  auto *Merge = BasicBlock::Create(Context, "merge", F);
  B.CreateCondBr(F->getArg(2), Loop, Fork);
  B.SetInsertPoint(Fork);
  auto *Null = ConstantPointerNull::get(cast<PointerType>(F->getReturnType()));
  B.CreateCondBr(B.CreateICmpEQ(F->getArg(3), Null), Guard, Right);
  B.SetInsertPoint(Guard);
  // The first base query stops at the unknown argument, leaving A pending.
  auto *LeftValue = B.CreateSelect(F->getArg(2), F->getArg(3), A);
  auto *RawPtr = PointerType::get(Context, 0);
  FunctionCallee Check = M.getOrInsertFunction(
      "jeandle.check_instanceof", B.getInt1Ty(), RawPtr, F->getReturnType());
  auto *Klass = ConstantExpr::getIntToPtr(B.getInt64(9), RawPtr);
  B.CreateCondBr(B.CreateCall(Check, {Klass, LeftValue}), Accepted, Fallback);
  B.SetInsertPoint(Accepted);
  B.CreateBr(Merge);
  B.SetInsertPoint(Right);
  auto *RightValue = B.CreateSelect(F->getArg(2), C, F->getArg(1));
  B.CreateBr(Merge);
  B.SetInsertPoint(Fallback);
  B.CreateRet(Null);
  B.SetInsertPoint(Merge);
  auto *Root = B.CreatePHI(F->getReturnType(), 2);
  Root->addIncoming(LeftValue, Accepted);
  Root->addIncoming(RightValue, Right);
  auto *Return = B.CreateRet(Root);
  ASSERT_FALSE(verifyFunction(*F));
  DominatorTree DT(*F);
  JavaType T = queryAt(Root, Return, DT);
  // C still needs the completed union of both cycle seeds, not an unknown
  // cached for every node seen during the first query.
  EXPECT_EQ(T.Klass, 5U);
  EXPECT_FALSE(T.Exact);
}

TEST_F(JavaBaseTypeTest, CycleRetainsOnlyCommonInterfaces) {
  VMCallbacks CB = *getVMCallbacks();
  CB.IsInterface = [](uintptr_t K) { return K == 7 || K == 8; };
  CB.GetSecondarySupers = [](uintptr_t K) {
    if (K == 9)
      return std::vector<uintptr_t>{7, 8};
    if (K == 22)
      return std::vector<uintptr_t>{7};
    return std::vector<uintptr_t>{};
  };
  registerVMCallbacks(CB);
  auto *A = B.CreatePHI(F->getReturnType(), 2);
  auto *C = B.CreatePHI(F->getReturnType(), 2);
  A->addIncoming(seed(0, 9, true), Entry);
  A->addIncoming(C, Loop);
  C->addIncoming(seed(1, 22, true), Entry);
  C->addIncoming(A, Loop);
  JavaType T = query(A);
  EXPECT_EQ(T.Klass, 5U);
  EXPECT_TRUE(T.Interfaces.contains(7));
  EXPECT_FALSE(T.Interfaces.contains(8));
  seed(1, 33, true);
  EXPECT_TRUE(query(C).Interfaces.empty());
}

TEST_F(JavaBaseTypeTest, CycleWithMixedExactSeedsStaysInexact) {
  auto *A = B.CreatePHI(F->getReturnType(), 2);
  auto *C = B.CreatePHI(F->getReturnType(), 2);
  A->addIncoming(seed(0, 9, true), Entry);
  A->addIncoming(C, Loop);
  C->addIncoming(seed(1, 9), Entry);
  C->addIncoming(A, Loop);
  JavaType T = query(A);
  EXPECT_EQ(T.Klass, 9U);
  EXPECT_FALSE(T.Exact);
  EXPECT_FALSE(query(C).Exact);
}

TEST_F(JavaBaseTypeTest, CycleThroughSelectCastsAndFreezeStaysUnknown) {
  auto *P = B.CreatePHI(F->getReturnType(), 2);
  P->addIncoming(seed(0, 9, true), Entry);
  auto *Raw = B.CreateAddrSpaceCast(P, PointerType::get(Context, 0));
  auto *Oop = B.CreateAddrSpaceCast(Raw, F->getReturnType());
  auto *Frozen = B.CreateFreeze(Oop);
  auto *Selected = B.CreateSelect(F->getArg(2), Frozen, seed(1, 22, true));
  P->addIncoming(Selected, Loop);
  EXPECT_TRUE(query(P).isUnknown());
  EXPECT_TRUE(query(Selected).isUnknown());
}

TEST_F(JavaBaseTypeTest, SeedlessCycleAndIncompletePhiStayUnknown) {
  auto *A = B.CreatePHI(F->getReturnType(), 1);
  auto *C = B.CreatePHI(F->getReturnType(), 1);
  A->addIncoming(C, Loop);
  C->addIncoming(A, Loop);
  EXPECT_TRUE(query(A).isUnknown());
  auto *Mixed =
      B.CreateSelect(F->getArg(2), seed(0, 9, true), B.CreateFreeze(A));
  EXPECT_TRUE(query(Mixed).isUnknown());
  auto *Incomplete = B.CreatePHI(F->getReturnType(), 0);
  EXPECT_TRUE(query(Incomplete).isUnknown());
  auto *WithIncomplete = B.CreateSelect(F->getArg(2), F->getArg(0), Incomplete);
  EXPECT_TRUE(query(WithIncomplete).isUnknown());
  Incomplete->addIncoming(F->getArg(0), Entry);
  EXPECT_EQ(query(WithIncomplete).Klass, 9U);
}

TEST_F(JavaBaseTypeTest, FreezeOutsideUnseededCycleMemberStaysUnknown) {
  Value *Seed = seed(0, 9, true);
  auto *A = B.CreatePHI(F->getReturnType(), 2);
  auto *C = B.CreatePHI(F->getReturnType(), 2);
  auto *Poisoned =
      SelectInst::Create(PoisonValue::get(B.getInt1Ty()), Seed,
                         seed(1, 9, true), "poisoned", Entry->getTerminator());
  A->addIncoming(Poisoned, Entry);
  A->addIncoming(C, Loop);
  C->addIncoming(A, Loop);
  C->addIncoming(C, Loop);
  auto *Mixed = B.CreateSelect(F->getArg(2), B.CreateFreeze(A), Seed);
  EXPECT_TRUE(query(Mixed).isUnknown());
}

} // namespace
