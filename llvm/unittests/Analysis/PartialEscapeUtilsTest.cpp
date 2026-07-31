//===- PartialEscapeUtilsTest.cpp - PEA helper unit tests -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/Jeandle/PartialEscapeUtils.h"
#include "llvm/ADT/Twine.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Jeandle/Deoptimization.h"
#include "llvm/IR/Jeandle/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/Support/SourceMgr.h"
#include "gtest/gtest.h"

#include <limits>
#include <string>

using namespace llvm;

namespace {

CallBase *parseOnlyCall(StringRef Body, LLVMContext &Ctx,
                        std::unique_ptr<Module> &M) {
  SMDiagnostic Err;
  std::string IR = R"(
    declare void @safepoint()
    define void @f(ptr addrspace(1) %oop, ptr %lock, ptr %origpc,
                   ptr addrspace(3) %narrow, i64 %wide, double %dbl) {
    entry:
      call void @safepoint() [ "deopt"()";
  IR.append(Body.data(), Body.size());
  IR += R"() ]
      ret void
    }
  )";
  M = parseAssemblyString(IR, Err, Ctx);
  if (!M)
    return nullptr;
  for (Instruction &I : instructions(*M->getFunction("f")))
    if (auto *CB = dyn_cast<CallBase>(&I))
      return CB;
  return nullptr;
}

uint64_t deoptEncoding(int32_t Index,
                       jeandle::DeoptValueEncoding::DeoptValueType ValueType,
                       jeandle::HotspotBasicType BasicType) {
  return (static_cast<uint64_t>(static_cast<uint32_t>(Index)) << 32) |
         (static_cast<uint64_t>(ValueType) << 16) |
         static_cast<uint64_t>(BasicType);
}

} // namespace

TEST(PartialEscapeUtilsTest, DistinguishesWholeObjectAndDerivedAliases) {
  LLVMContext Ctx;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseAssemblyString(
      R"(
        target datalayout = "e-p:64:64-p1:64:64"
        define void @f(ptr addrspace(1) %oop, ptr addrspace(1) %slot) {
        entry:
          %loaded = load ptr addrspace(1), ptr addrspace(1) %slot
          %derived = getelementptr i8, ptr addrspace(1) %oop, i64 8
          ret void
        }
      )",
      Err, Ctx);
  ASSERT_NE(M, nullptr);
  Function *F = M->getFunction("f");
  ASSERT_NE(F, nullptr);
  Value *Loaded = F->getValueSymbolTable()->lookup("loaded");
  Value *Derived = F->getValueSymbolTable()->lookup("derived");
  ASSERT_NE(Loaded, nullptr);
  ASSERT_NE(Derived, nullptr);

  jeandle::PEABlockState State;
  State.addObject(0, jeandle::ObjectState());
  State.addObject(1, jeandle::ObjectState());
  jeandle::AliasMap Aliases;
  Aliases.addVirtualAlias(Loaded, 0, /*IsWholeObject=*/true);
  Aliases.addVirtualAlias(Derived, 1, /*IsWholeObject=*/false);

  auto LoadedWhole = jeandle::pea::resolveVirtualIdentity(
      Loaded, State, Aliases, M->getDataLayout(),
      jeandle::pea::VirtualIdentityMode::WholeObject);
  EXPECT_TRUE(LoadedWhole.isDefined());
  EXPECT_EQ(LoadedWhole.getObjectID(), 0u);

  auto DerivedWhole = jeandle::pea::resolveVirtualIdentity(
      Derived, State, Aliases, M->getDataLayout(),
      jeandle::pea::VirtualIdentityMode::WholeObject);
  EXPECT_FALSE(DerivedWhole.isDefined());

  auto DerivedBase = jeandle::pea::resolveVirtualIdentity(
      Derived, State, Aliases, M->getDataLayout(),
      jeandle::pea::VirtualIdentityMode::BaseObject);
  EXPECT_TRUE(DerivedBase.isDefined());
  EXPECT_EQ(DerivedBase.getObjectID(), 1u);
}

TEST(PartialEscapeUtilsTest, ParsesCompleteSemanticDeoptBundle) {
  LLVMContext Ctx;
  std::unique_ptr<Module> M;
  CallBase *CB = parseOnlyCall(
      R"(
        i64 0, i32 10, i32 10,
        i64 30065033229, i64 4096, i32 2,
          i64 68719476747, i64 %wide,
          i64 103079739404, i32 9,
        i64 38654967820, i64 8192, i32 0,
        i64 30065295372, i32 7,
        i64 4294967307, i64 %wide,
        i64 65543, double %dbl,
        i64 4295163916, i32 9, ptr %lock,
        i64 327695, ptr %origpc,
        i64 393233, i64 12288,
        i64 1, i32 20, i32 20,
        i64 38655229964, i32 9,
        i64 65548, ptr addrspace(1) %oop,
        i64 196620, ptr addrspace(1) %oop, ptr %lock,
        i64 458768, ptr addrspace(3) %narrow)",
      Ctx, M);
  ASSERT_NE(CB, nullptr);

  jeandle::pea::DeoptBundleParseResult Result =
      jeandle::pea::parseDeoptBundle(*CB);
  ASSERT_TRUE(Result.Bundle.has_value());
  const jeandle::pea::ParsedDeoptBundle &Bundle = *Result.Bundle;

  ASSERT_EQ(Bundle.Descriptors.size(), 2u);
  EXPECT_EQ(Bundle.Descriptors[0].WireID, 7);
  EXPECT_TRUE(Bundle.Descriptors[0].IsArray);
  EXPECT_EQ(Bundle.Descriptors[0].Klass, 4096u);
  ASSERT_EQ(Bundle.Descriptors[0].Fields.size(), 2u);
  EXPECT_EQ(Bundle.Descriptors[0].Fields[0].Offset, 16);
  EXPECT_EQ(Bundle.Descriptors[0].Fields[0].Encoding.BasicType,
            jeandle::T_LONG);
  EXPECT_FALSE(Bundle.Descriptors[0].Fields[0].TargetWireID.has_value());
  EXPECT_EQ(Bundle.Descriptors[0].Fields[0].EncodingCell.OperandIndex, 6u);
  EXPECT_EQ(Bundle.Descriptors[0].Fields[0].ValueCell.OperandIndex, 7u);
  EXPECT_EQ(Bundle.Descriptors[0].Fields[1].Offset, 24);
  ASSERT_TRUE(Bundle.Descriptors[0].Fields[1].TargetWireID.has_value());
  EXPECT_EQ(*Bundle.Descriptors[0].Fields[1].TargetWireID, 9);
  EXPECT_EQ(Bundle.Descriptors[1].WireID, 9);
  EXPECT_FALSE(Bundle.Descriptors[1].IsArray);

  ASSERT_EQ(Bundle.Scopes.size(), 2u);
  EXPECT_FALSE(Bundle.Scopes[0].Method.has_value());
  EXPECT_TRUE(Bundle.Scopes[0].ShouldReexecute.has_value());
  EXPECT_EQ(Bundle.Scopes[0].BCI, 10);
  ASSERT_EQ(Bundle.Scopes[0].Locals.size(), 2u);
  EXPECT_EQ(Bundle.Scopes[0].Locals[0].Encoding.ValueType,
            jeandle::DeoptValueEncoding::VORefLocalType);
  EXPECT_EQ(Bundle.Scopes[0].Locals[0].PhysicalSlot, 0u);
  EXPECT_EQ(Bundle.Scopes[0].Locals[0].SlotWidth, 1u);
  EXPECT_EQ(Bundle.Scopes[0].Locals[1].Encoding.BasicType, jeandle::T_LONG);
  EXPECT_EQ(Bundle.Scopes[0].Locals[1].PhysicalSlot, 1u);
  EXPECT_EQ(Bundle.Scopes[0].Locals[1].SlotWidth, 2u);
  ASSERT_EQ(Bundle.Scopes[0].Stack.size(), 1u);
  EXPECT_EQ(Bundle.Scopes[0].Stack[0].Encoding.BasicType, jeandle::T_DOUBLE);
  EXPECT_EQ(Bundle.Scopes[0].Stack[0].PhysicalSlot, 0u);
  EXPECT_EQ(Bundle.Scopes[0].Stack[0].SlotWidth, 2u);
  ASSERT_EQ(Bundle.Scopes[0].Monitors.size(), 1u);
  EXPECT_TRUE(Bundle.Scopes[0].Monitors[0].Eliminated);
  ASSERT_TRUE(Bundle.Scopes[0].Monitors[0].OwnerWireID.has_value());
  EXPECT_EQ(*Bundle.Scopes[0].Monitors[0].OwnerWireID, 9);
  EXPECT_TRUE(Bundle.Scopes[0].OrigPc.has_value());

  ASSERT_TRUE(Bundle.Scopes[1].Method.has_value());
  EXPECT_EQ(Bundle.Scopes[1].Method->Method, 12288u);
  EXPECT_EQ(Bundle.Scopes[1].BCI, 20);
  ASSERT_EQ(Bundle.Scopes[1].Locals.size(), 1u);
  ASSERT_EQ(Bundle.Scopes[1].Stack.size(), 1u);
  ASSERT_EQ(Bundle.Scopes[1].Monitors.size(), 1u);
  EXPECT_FALSE(Bundle.Scopes[1].Monitors[0].Eliminated);
  EXPECT_FALSE(Bundle.Scopes[1].OrigPc.has_value());

  ASSERT_EQ(Bundle.NarrowOopMarkers.size(), 1u);
  EXPECT_EQ(Bundle.NarrowOopMarkers[0].ValueCell.OperandIndex, 37u);
  EXPECT_EQ(Bundle.Fingerprint.Cells.size(), Bundle.OriginalInputs.size());
  EXPECT_TRUE(jeandle::pea::matchesParsedDeoptBundle(Bundle, *CB));

  SmallVector<Value *, 40> Serialized;
  ASSERT_TRUE(jeandle::pea::copyParsedDeoptBundleInputs(Bundle, Serialized));
  ASSERT_EQ(Serialized.size(), Bundle.OriginalInputs.size());
  for (unsigned I = 0; I < Serialized.size(); ++I)
    EXPECT_EQ(Serialized[I], static_cast<Value *>(Bundle.OriginalInputs[I]));
}

TEST(PartialEscapeUtilsTest, AcceptsSimplifiedHeaderWithoutOrigPc) {
  LLVMContext Ctx;
  std::unique_ptr<Module> M;
  CallBase *CB = parseOnlyCall(R"(i32 5, i32 5, i64 10, i32 17)", Ctx, M);
  ASSERT_NE(CB, nullptr);

  jeandle::pea::DeoptBundleParseResult Result =
      jeandle::pea::parseDeoptBundle(*CB);
  ASSERT_TRUE(Result.Bundle.has_value());
  ASSERT_EQ(Result.Bundle->Scopes.size(), 1u);
  EXPECT_FALSE(Result.Bundle->Scopes[0].ShouldReexecute.has_value());
  EXPECT_EQ(Result.Bundle->Scopes[0].BCI, 5);
  EXPECT_FALSE(Result.Bundle->Scopes[0].OrigPc.has_value());
  ASSERT_EQ(Result.Bundle->Scopes[0].Locals.size(), 1u);
}

TEST(PartialEscapeUtilsTest, LongAndDoubleUseOneWirePairEach) {
  LLVMContext Ctx;
  std::unique_ptr<Module> M;
  CallBase *CB = parseOnlyCall(
      R"(i32 5, i32 5,
         i64 42949935116, i64 4096, i32 2,
           i64 34359738379, i64 %wide,
           i64 51539607559, double %dbl)",
      Ctx, M);
  ASSERT_NE(CB, nullptr);

  jeandle::pea::DeoptBundleParseResult Result =
      jeandle::pea::parseDeoptBundle(*CB);
  ASSERT_TRUE(Result.Bundle.has_value());
  ASSERT_EQ(Result.Bundle->Descriptors.size(), 1u);
  ASSERT_EQ(Result.Bundle->Descriptors[0].Fields.size(), 2u);
  EXPECT_EQ(Result.Bundle->Descriptors[0].Fields[0].Encoding.BasicType,
            jeandle::T_LONG);
  EXPECT_EQ(Result.Bundle->Descriptors[0].Fields[1].Encoding.BasicType,
            jeandle::T_DOUBLE);
  EXPECT_EQ(Result.Bundle->Descriptors[0].Fields[1].ValueCell.OperandIndex, 8u);
}

TEST(PartialEscapeUtilsTest, AcceptsDenseMapSentinelEncodingValues) {
  LLVMContext Ctx;
  std::unique_ptr<Module> M;
  const int32_t WireID = std::numeric_limits<int32_t>::max();
  const int32_t FieldOffset = WireID - 1;
  const uint64_t Descriptor = deoptEncoding(
      WireID, jeandle::DeoptValueEncoding::ScalarValueType, jeandle::T_OBJECT);
  const uint64_t Field = deoptEncoding(
      FieldOffset, jeandle::DeoptValueEncoding::LocalType, jeandle::T_INT);
  const uint64_t Root = deoptEncoding(
      WireID, jeandle::DeoptValueEncoding::VORefLocalType, jeandle::T_OBJECT);
  CallBase *CB =
      parseOnlyCall((Twine("i32 5, i32 5, i64 ") + Twine(Descriptor) +
                     ", i64 4096, i32 1, i64 " + Twine(Field) +
                     ", i32 17, i64 " + Twine(Root) + ", i32 " + Twine(WireID))
                        .str(),
                    Ctx, M);
  ASSERT_NE(CB, nullptr);

  jeandle::pea::DeoptBundleParseResult Result =
      jeandle::pea::parseDeoptBundle(*CB);
  ASSERT_TRUE(Result.Bundle.has_value());
  ASSERT_EQ(Result.Bundle->Descriptors.size(), 1u);
  EXPECT_EQ(Result.Bundle->Descriptors[0].WireID, WireID);
  ASSERT_EQ(Result.Bundle->Descriptors[0].Fields.size(), 1u);
  EXPECT_EQ(Result.Bundle->Descriptors[0].Fields[0].Offset, FieldOffset);
}

TEST(PartialEscapeUtilsTest, RejectsMalformedSemanticDeoptGrammar) {
  struct Case {
    const char *Name;
    std::string Inputs;
    jeandle::pea::DeoptBundleParseErrorCode Expected;
  };

  const uint64_t ScalarObject = deoptEncoding(
      1, jeandle::DeoptValueEncoding::ScalarValueType, jeandle::T_OBJECT);
  const uint64_t LocalObject = deoptEncoding(
      0, jeandle::DeoptValueEncoding::LocalType, jeandle::T_OBJECT);
  const uint64_t VORefLocal = deoptEncoding(
      1, jeandle::DeoptValueEncoding::VORefLocalType, jeandle::T_OBJECT);
  const uint64_t OrigPc = deoptEncoding(
      0, jeandle::DeoptValueEncoding::OrigPcSlotType, jeandle::T_ADDRESS);
  const uint64_t Method = deoptEncoding(
      0, jeandle::DeoptValueEncoding::MethodType, jeandle::T_METADATA);
  const uint64_t Narrow =
      deoptEncoding(0, jeandle::DeoptValueEncoding::NarrowOopMarkerType,
                    jeandle::T_NARROWOOP);
  const uint64_t NegativeScalarObject = deoptEncoding(
      -1, jeandle::DeoptValueEncoding::ScalarValueType, jeandle::T_OBJECT);
  const uint64_t NegativeField =
      deoptEncoding(-1, jeandle::DeoptValueEncoding::LocalType, jeandle::T_INT);
  const uint64_t MismatchedVORefLocal = deoptEncoding(
      2, jeandle::DeoptValueEncoding::VORefLocalType, jeandle::T_OBJECT);
  const uint64_t InvalidMonitorIndex = deoptEncoding(
      2, jeandle::DeoptValueEncoding::MonitorType, jeandle::T_OBJECT);
  const uint64_t InvalidMonitorType = deoptEncoding(
      0, jeandle::DeoptValueEncoding::MonitorType, jeandle::T_INT);

  SmallVector<Case, 20> Cases{
      {"mismatched bci", "i32 5, i32 6",
       jeandle::pea::DeoptBundleParseErrorCode::MismatchedBCI},
      {"truncated descriptor",
       (Twine("i32 5, i32 5, i64 ") + Twine(ScalarObject) + ", i64 4096").str(),
       jeandle::pea::DeoptBundleParseErrorCode::TruncatedRecord},
      {"duplicate descriptor id",
       (Twine("i32 5, i32 5, i64 ") + Twine(ScalarObject) +
        ", i64 4096, i32 0, i64 " + Twine(ScalarObject) + ", i64 4096, i32 0")
           .str(),
       jeandle::pea::DeoptBundleParseErrorCode::DuplicateDescriptorID},
      {"negative descriptor id",
       (Twine("i32 5, i32 5, i64 ") + Twine(NegativeScalarObject) +
        ", i64 4096, i32 0")
           .str(),
       jeandle::pea::DeoptBundleParseErrorCode::InvalidEncoding},
      {"negative field offset",
       (Twine("i32 5, i32 5, i64 ") + Twine(ScalarObject) +
        ", i64 4096, i32 1, i64 " + Twine(NegativeField) + ", i32 1")
           .str(),
       jeandle::pea::DeoptBundleParseErrorCode::InvalidEncoding},
      {"duplicate field offset",
       (Twine("i32 5, i32 5, i64 ") + Twine(ScalarObject) +
        ", i64 4096, i32 2, i64 34359738378, i32 1, "
        "i64 34359738378, i32 2")
           .str(),
       jeandle::pea::DeoptBundleParseErrorCode::DuplicateFieldOffset},
      {"dangling voref",
       (Twine("i32 5, i32 5, i64 ") + Twine(VORefLocal) + ", i32 1").str(),
       jeandle::pea::DeoptBundleParseErrorCode::DanglingVORef},
      {"scope voref ids disagree",
       (Twine("i32 5, i32 5, i64 ") + Twine(ScalarObject) +
        ", i64 4096, i32 0, i64 " + Twine(MismatchedVORefLocal) + ", i32 1")
           .str(),
       jeandle::pea::DeoptBundleParseErrorCode::InvalidSemanticValue},
      {"descriptor in inline scope",
       (Twine("i32 5, i32 5, i64 ") + Twine(Method) +
        ", i64 12288, i32 6, i32 6, i64 " + Twine(ScalarObject) +
        ", i64 4096, i32 0")
           .str(),
       jeandle::pea::DeoptBundleParseErrorCode::DescriptorNotInRootPool},
      {"duplicate root orig pc",
       (Twine("i32 5, i32 5, i64 ") + Twine(OrigPc) + ", ptr %origpc, i64 " +
        Twine(OrigPc) + ", ptr %origpc")
           .str(),
       jeandle::pea::DeoptBundleParseErrorCode::InvalidOrigPc},
      {"orig pc in inline scope",
       (Twine("i32 5, i32 5, i64 ") + Twine(Method) +
        ", i64 12288, i32 6, i32 6, i64 " + Twine(OrigPc) + ", ptr %origpc")
           .str(),
       jeandle::pea::DeoptBundleParseErrorCode::InvalidOrigPc},
      {"narrow marker with wide oop",
       (Twine("i32 5, i32 5, i64 ") + Twine(Narrow) + ", ptr addrspace(1) %oop")
           .str(),
       jeandle::pea::DeoptBundleParseErrorCode::InvalidNarrowOopMarker},
      {"value after narrow tail",
       (Twine("i32 5, i32 5, i64 ") + Twine(Narrow) +
        ", ptr addrspace(3) %narrow, i64 " + Twine(LocalObject) +
        ", ptr addrspace(1) %oop")
           .str(),
       jeandle::pea::DeoptBundleParseErrorCode::InvalidNarrowOopMarker},
      {"non-null constant oop",
       (Twine("i32 5, i32 5, i64 ") + Twine(LocalObject) +
        ", ptr addrspace(1) inttoptr (i64 1 to ptr addrspace(1))")
           .str(),
       jeandle::pea::DeoptBundleParseErrorCode::InvalidSemanticValue},
      {"method value is not constant",
       (Twine("i32 5, i32 5, i64 ") + Twine(Method) + ", i64 %wide").str(),
       jeandle::pea::DeoptBundleParseErrorCode::InvalidMethodMarker},
      {"invalid monitor index",
       (Twine("i32 5, i32 5, i64 ") + Twine(InvalidMonitorIndex) +
        ", ptr addrspace(1) %oop, ptr %lock")
           .str(),
       jeandle::pea::DeoptBundleParseErrorCode::InvalidMonitor},
      {"invalid monitor basic type",
       (Twine("i32 5, i32 5, i64 ") + Twine(InvalidMonitorType) +
        ", ptr addrspace(1) %oop, ptr %lock")
           .str(),
       jeandle::pea::DeoptBundleParseErrorCode::InvalidMonitor},
      {"truncated monitor",
       (Twine("i32 5, i32 5, i64 ") +
        Twine(deoptEncoding(0, jeandle::DeoptValueEncoding::MonitorType,
                            jeandle::T_OBJECT)) +
        ", ptr addrspace(1) %oop")
           .str(),
       jeandle::pea::DeoptBundleParseErrorCode::TruncatedRecord},
  };

  for (const Case &C : Cases) {
    SCOPED_TRACE(C.Name);
    LLVMContext Ctx;
    std::unique_ptr<Module> M;
    CallBase *CB = parseOnlyCall(C.Inputs, Ctx, M);
    ASSERT_NE(CB, nullptr);
    jeandle::pea::DeoptBundleParseResult Result =
        jeandle::pea::parseDeoptBundle(*CB);
    ASSERT_FALSE(Result.Bundle.has_value());
    EXPECT_EQ(Result.Error.Code, C.Expected);
  }
}

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
