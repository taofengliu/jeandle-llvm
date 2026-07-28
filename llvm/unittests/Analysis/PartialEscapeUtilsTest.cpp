//===- PartialEscapeUtilsTest.cpp - PEA helper unit tests -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/Jeandle/PartialEscapeUtils.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Jeandle/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/Support/SourceMgr.h"
#include "gtest/gtest.h"

#include <limits>

using namespace llvm;

TEST(PartialEscapeUtilsTest, TraversesTransparentCarriersToSemanticUse) {
  LLVMContext Ctx;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseAssemblyString(R"(
    @annotation = private constant [2 x i8] c"x\00"
    @file = private constant [2 x i8] c"f\00"

    declare void @sink(ptr addrspace(1))
    declare ptr addrspace(1)
        @llvm.launder.invariant.group.p1(ptr addrspace(1))
    declare ptr addrspace(1)
        @llvm.strip.invariant.group.p1(ptr addrspace(1))
    declare ptr addrspace(1) @llvm.ptr.annotation.p1.p0(
        ptr addrspace(1), ptr, ptr, i32, ptr)

    define void @f(ptr addrspace(1) %root, i1 %cond) {
    entry:
      %frozen = freeze ptr addrspace(1) %root
      %laundered = call ptr addrspace(1)
          @llvm.launder.invariant.group.p1(ptr addrspace(1) %frozen)
      %stripped = call ptr addrspace(1)
          @llvm.strip.invariant.group.p1(ptr addrspace(1) %laundered)
      %annotated = call ptr addrspace(1) @llvm.ptr.annotation.p1.p0(
          ptr addrspace(1) %stripped, ptr @annotation, ptr @file, i32 1,
          ptr null)
      br i1 %cond, label %left, label %right

    left:
      br label %merge

    right:
      br label %merge

    merge:
      %phi = phi ptr addrspace(1)
          [ %annotated, %left ], [ %root, %right ]
      %selected = select i1 %cond, ptr addrspace(1) %phi,
          ptr addrspace(1) %frozen
      %narrow = addrspacecast ptr addrspace(1) %selected to ptr addrspace(3)
      %wide = addrspacecast ptr addrspace(3) %narrow to ptr addrspace(1)
      %field = getelementptr i8, ptr addrspace(1) %wide, i64 8
      call void @sink(ptr addrspace(1) %field)
      ret void
    }
  )",
                                                  Err, Ctx);
  ASSERT_TRUE(M);
  Function *F = M->getFunction("f");
  ASSERT_NE(F, nullptr);

  unsigned SemanticUses = 0;
  EXPECT_TRUE(
      jeandle::pea::hasUnremovedSemanticUses(F->getArg(0), [&](const Use &U) {
        auto *CB = dyn_cast<CallBase>(U.getUser());
        if (CB && CB->getCalledFunction() &&
            CB->getCalledFunction()->getName() == "sink")
          ++SemanticUses;
        return false;
      }));
  EXPECT_EQ(SemanticUses, 1u);
}

TEST(PartialEscapeUtilsTest, RemovedMemoryUsesAreExempt) {
  LLVMContext Ctx;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseAssemblyString(R"(
    define i32 @f(ptr addrspace(1) %root) {
    entry:
      %frozen = freeze ptr addrspace(1) %root
      %field = getelementptr i8, ptr addrspace(1) %frozen, i64 8
      store atomic i32 42, ptr addrspace(1) %field unordered, align 4
      %value = load atomic i32, ptr addrspace(1) %field unordered, align 4
      ret i32 %value
    }
  )",
                                                  Err, Ctx);
  ASSERT_TRUE(M);
  Function *F = M->getFunction("f");
  ASSERT_NE(F, nullptr);

  unsigned RemovedUses = 0;
  EXPECT_FALSE(
      jeandle::pea::hasUnremovedSemanticUses(F->getArg(0), [&](const Use &U) {
        bool IsRemovedMemoryUse =
            isa<LoadInst>(U.getUser()) || isa<StoreInst>(U.getUser());
        RemovedUses += IsRemovedMemoryUse;
        return IsRemovedMemoryUse;
      }));
  EXPECT_EQ(RemovedUses, 2u);
}

TEST(PartialEscapeUtilsTest, RewrittenDeoptUseIsExempt) {
  LLVMContext Ctx;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseAssemblyString(R"(
    declare void @safepoint()

    define void @f(ptr addrspace(1) %root) {
    entry:
      %frozen = freeze ptr addrspace(1) %root
      call void @safepoint()
          [ "deopt"(i32 99, ptr addrspace(1) %frozen) ]
      ret void
    }
  )",
                                                  Err, Ctx);
  ASSERT_TRUE(M);
  Function *F = M->getFunction("f");
  ASSERT_NE(F, nullptr);

  unsigned RewrittenDeoptUses = 0;
  EXPECT_FALSE(
      jeandle::pea::hasUnremovedSemanticUses(F->getArg(0), [&](const Use &U) {
        auto *CB = dyn_cast<CallBase>(U.getUser());
        bool IsRewrittenDeopt = CB && CB->isBundleOperand(U.getOperandNo()) &&
                                CB->getOperandBundleForOperand(U.getOperandNo())
                                    .isDeoptOperandBundle();
        RewrittenDeoptUses += IsRewrittenDeopt;
        return IsRewrittenDeopt;
      }));
  EXPECT_EQ(RewrittenDeoptUses, 1u);
}

TEST(PartialEscapeUtilsTest, RejectsOverflowingAccumulatedOffsets) {
  LLVMContext Ctx;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseAssemblyString(R"(
    target datalayout = "e-p1:64:64-p3:64:64"

    define void @f(ptr addrspace(1) %root) {
    entry:
      %near.max = getelementptr i8, ptr addrspace(1) %root,
          i64 9223372036854775805
      %max = getelementptr i8, ptr addrspace(1) %root,
          i64 9223372036854775807
      %max.plus.one = getelementptr i8, ptr addrspace(1) %max, i64 1
      %densemap.tombstone = getelementptr i8, ptr addrspace(1) %root,
          i64 9223372036854775806
      %min = getelementptr i8, ptr addrspace(1) %root,
          i64 -9223372036854775808
      %min.minus.one = getelementptr i8, ptr addrspace(1) %min, i64 -1
      ret void
    }
  )",
                                                  Err, Ctx);
  ASSERT_TRUE(M);
  Function *F = M->getFunction("f");
  ASSERT_NE(F, nullptr);
  ValueSymbolTable *Symbols = F->getValueSymbolTable();
  ASSERT_NE(Symbols, nullptr);

  auto NearMax = jeandle::pea::resolveFieldOffset(Symbols->lookup("near.max"),
                                                  M->getDataLayout());
  ASSERT_TRUE(NearMax);
  EXPECT_EQ(*NearMax, std::numeric_limits<int64_t>::max() - 2);
  EXPECT_FALSE(jeandle::pea::resolveFieldOffset(Symbols->lookup("max"),
                                                M->getDataLayout()));
  EXPECT_FALSE(jeandle::pea::resolveFieldOffset(Symbols->lookup("max.plus.one"),
                                                M->getDataLayout()));
  EXPECT_FALSE(jeandle::pea::resolveFieldOffset(
      Symbols->lookup("densemap.tombstone"), M->getDataLayout()));
  EXPECT_FALSE(jeandle::pea::resolveFieldOffset(
      Symbols->lookup("min.minus.one"), M->getDataLayout()));
}

TEST(PartialEscapeUtilsTest, RejectsUnrepresentableFieldRangeEndpoint) {
  LLVMContext Ctx;
  Module M("field-range", Ctx);
  M.setDataLayout("e-p1:64:64-p3:64:64");
  jeandle::VirtualObject VO(jeandle::InvalidObjectID,
                            jeandle::VirtualObject::Instance, nullptr);

  EXPECT_EQ(VO.getOrCreateFieldIndex(std::numeric_limits<int64_t>::max(),
                                     Type::getInt8Ty(Ctx), M.getDataLayout()),
            -1);
}

TEST(PartialEscapeUtilsTest, CheckedOffsetArithmeticReportsUnknown) {
  constexpr int64_t Min = std::numeric_limits<int64_t>::min();
  constexpr int64_t Max = std::numeric_limits<int64_t>::max();

  EXPECT_FALSE(jeandle::pea::checkedOffsetAdd(Max, 1));
  EXPECT_FALSE(jeandle::pea::checkedOffsetSub(Min, 1));
  EXPECT_FALSE(
      jeandle::pea::checkedArrayElementOffset(16, 2147483649LL, 4294967295ULL));
  EXPECT_FALSE(jeandle::pea::checkedRangesOverlap(Max, 1, 0, 1));

  auto Overlap = jeandle::pea::checkedRangesOverlap(8, 4, 10, 2);
  ASSERT_TRUE(Overlap);
  EXPECT_TRUE(*Overlap);
}

TEST(PartialEscapeUtilsTest, RejectsWideSlotScanEndpointOverflow) {
  constexpr int64_t Start = std::numeric_limits<int64_t>::max() - 1;
  constexpr unsigned WideBytes = 4;

  bool SawUnknown = false;
  for (unsigned Byte = 1; Byte < WideBytes; ++Byte) {
    std::optional<int64_t> Adjacent =
        jeandle::pea::checkedOffsetAdd(Start, static_cast<int64_t>(Byte));
    if (!Adjacent || !jeandle::pea::isUsableFieldOffset(*Adjacent)) {
      SawUnknown = true;
      break;
    }
  }
  EXPECT_TRUE(SawUnknown);
}

TEST(PartialEscapeUtilsTest, UsesGEPIndexWidthNotPointerWidth) {
  LLVMContext Ctx;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseAssemblyString(R"(
    target datalayout = "e-p1:128:128:128:64"

    define void @f(ptr addrspace(1) %root) {
    entry:
      %field = getelementptr i8, ptr addrspace(1) %root,
          i128 18446744073709551624
      ret void
    }
  )",
                                                  Err, Ctx);
  ASSERT_TRUE(M);
  Value *Field = M->getFunction("f")->getValueSymbolTable()->lookup("field");
  ASSERT_NE(Field, nullptr);

  std::optional<int64_t> Offset =
      jeandle::pea::resolveFieldOffset(Field, M->getDataLayout());
  ASSERT_TRUE(Offset);
  EXPECT_EQ(*Offset, 8);
}

TEST(PartialEscapeUtilsTest, RejectsOversizedPointerFieldWidth) {
  LLVMContext Ctx;
  Module M("oversized-pointer-field", Ctx);
  M.setDataLayout("e-p1:2048:2048-p3:2048:2048");
  jeandle::VirtualObject VO(jeandle::InvalidObjectID,
                            jeandle::VirtualObject::Instance, nullptr);

  Type *PointerField =
      PointerType::get(Ctx, jeandle::AddrSpace::JavaHeapAddrSpace);
  EXPECT_EQ(VO.getOrCreateFieldIndex(8, PointerField, M.getDataLayout()), -1);
}

TEST(PartialEscapeUtilsTest, RejectsIllegalAtomicReplayTypesAtFieldProducer) {
  LLVMContext Ctx;
  Module M("atomic-replay-field-types", Ctx);
  M.setDataLayout("e-p1:64:64-p3:64:64");
  const DataLayout &DL = M.getDataLayout();

  auto FieldIndexFor = [&](Type *Ty) {
    jeandle::VirtualObject VO(jeandle::InvalidObjectID,
                              jeandle::VirtualObject::Instance, nullptr);
    return VO.getOrCreateFieldIndex(8, Ty, DL);
  };

  SmallVector<Type *, 5> IllegalTypes{
      IntegerType::get(Ctx, 24), Type::getX86_FP80Ty(Ctx),
      FixedVectorType::get(Type::getInt32Ty(Ctx), 3),
      ScalableVectorType::get(Type::getInt32Ty(Ctx), 2),
      StructType::get(Type::getInt32Ty(Ctx), Type::getInt32Ty(Ctx))};
  for (Type *Ty : IllegalTypes) {
    EXPECT_FALSE(jeandle::pea::isLegalMaterializationAtomicType(Ty, DL));
    EXPECT_EQ(FieldIndexFor(Ty), -1);
  }

  SmallVector<Type *, 8> LegalTypes{
      Type::getInt8Ty(Ctx),
      Type::getInt32Ty(Ctx),
      Type::getFloatTy(Ctx),
      Type::getDoubleTy(Ctx),
      PointerType::get(Ctx, jeandle::AddrSpace::JavaHeapAddrSpace),
      FixedVectorType::get(
          PointerType::get(Ctx, jeandle::AddrSpace::JavaHeapAddrSpace), 2),
      FixedVectorType::get(Type::getInt32Ty(Ctx), 4),
      FixedVectorType::get(Type::getInt1Ty(Ctx), 8)};
  for (Type *Ty : LegalTypes) {
    EXPECT_TRUE(jeandle::pea::isLegalMaterializationAtomicType(Ty, DL));
    EXPECT_GE(FieldIndexFor(Ty), 0);
  }
}
