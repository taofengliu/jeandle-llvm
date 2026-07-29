//===- DeoptPoolBundleLoweringTest.cpp - Deopt bundle lowering tests ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/Jeandle/DeoptPoolBundleLowering.h"
#include "llvm/Analysis/Jeandle/DeoptPoolPlanner.h"
#include "llvm/Analysis/Jeandle/PartialEscapeUtils.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Jeandle/Deoptimization.h"
#include "llvm/IR/Jeandle/Metadata.h"
#include "llvm/IR/Module.h"
#include "gtest/gtest.h"

#include <initializer_list>

using namespace llvm;
using namespace llvm::jeandle::pea;

namespace {

uint64_t encoding(int32_t Index,
                  jeandle::DeoptValueEncoding::DeoptValueType ValueType,
                  jeandle::HotspotBasicType BasicType) {
  return jeandle::DeoptValueEncoding(Index, ValueType, BasicType).encode();
}

class BundleIR {
public:
  LLVMContext Context;
  Module M{"deopt-bundle-lowering", Context};
  IRBuilder<> Builder{Context};
  Function *F = nullptr;
  Value *Object = nullptr;
  Value *OtherObject = nullptr;
  Value *Int = nullptr;
  Value *Long = nullptr;
  Value *Double = nullptr;
  Value *Lock = nullptr;

  BundleIR() {
    Type *WideOop =
        PointerType::get(Context, jeandle::AddrSpace::JavaHeapAddrSpace);
    FunctionType *FT = FunctionType::get(
        Builder.getVoidTy(),
        {WideOop, WideOop, Builder.getInt32Ty(), Builder.getInt64Ty(),
         Builder.getDoubleTy(), Builder.getPtrTy()},
        false);
    F = Function::Create(FT, Function::ExternalLinkage, "f", M);
    auto Arg = F->arg_begin();
    Object = &*Arg++;
    OtherObject = &*Arg++;
    Int = &*Arg++;
    Long = &*Arg++;
    Double = &*Arg++;
    Lock = &*Arg++;
    Object->setName("object");
    OtherObject->setName("other.object");
    Int->setName("int");
    Long->setName("long");
    Double->setName("double");
    Lock->setName("lock");
    BasicBlock *Entry = BasicBlock::Create(Context, "entry", F);
    Builder.SetInsertPoint(Entry);
  }

  CallBase *finish(ArrayRef<Value *> Inputs) {
    FunctionCallee Safepoint = M.getOrInsertFunction(
        "safepoint", FunctionType::get(Builder.getVoidTy(), false));
    OperandBundleDef Deopt("deopt", Inputs);
    CallBase *CB = Builder.CreateCall(Safepoint, {}, {Deopt});
    Builder.CreateRetVoid();
    return CB;
  }

  ConstantInt *i32(uint32_t Value) { return Builder.getInt32(Value); }

  ConstantInt *i64(uint64_t Value) { return Builder.getInt64(Value); }
};

ParsedDeoptBundle parse(CallBase &CB) {
  DeoptBundleParseResult Result = parseDeoptBundle(CB);
  EXPECT_TRUE(Result.Bundle.has_value());
  return std::move(*Result.Bundle);
}

FinalDeoptPoolGraphPlan plan(const DeoptPoolPlannerInput &Input) {
  DeoptPoolPlannerResult Result = planDeoptPool(Input);
  EXPECT_TRUE(Result.Plan.has_value());
  EXPECT_FALSE(Result.Error.has_value());
  EXPECT_TRUE(Result.FallbackSeeds.empty());
  return std::move(*Result.Plan);
}

LegacyDeoptPoolNode
legacyNode(uint32_t WireID, uint64_t Klass,
           std::initializer_list<DeoptPoolFieldInput> Fields) {
  LegacyDeoptPoolNode Node;
  Node.WireID = WireID;
  Node.Klass = Klass;
  Node.Fields.append(Fields.begin(), Fields.end());
  return Node;
}

CurrentDeoptPoolNode
currentNode(CurrentDeoptNodeID ID, uint64_t Klass,
            std::initializer_list<DeoptPoolFieldInput> Fields = {}) {
  CurrentDeoptPoolNode Node;
  Node.ID = ID;
  Node.Klass = Klass;
  Node.Fields.append(Fields.begin(), Fields.end());
  return Node;
}

DeoptPoolFieldInput scalarField(DeoptPoolSemanticCellID Cell, int64_t Offset,
                                jeandle::HotspotBasicType BasicType,
                                uint64_t Token) {
  return DeoptPoolFieldInput::scalar(Cell, Offset, BasicType, Token);
}

DeoptPoolFieldInput refField(DeoptPoolSemanticCellID Cell, int64_t Offset,
                             uint32_t LegacyWireID) {
  return DeoptPoolFieldInput::reference(Cell, Offset,
                                        DeoptPoolNodeRef::legacy(LegacyWireID));
}

DeoptPoolRootInput legacyRoot(DeoptPoolSemanticCellID Cell,
                              DeoptPoolRootKind Kind, uint32_t WireID) {
  return DeoptPoolRootInput::reference(Cell, Kind,
                                       DeoptPoolNodeRef::legacy(WireID));
}

DeoptPoolRootInput scalarRoot(DeoptPoolSemanticCellID Cell,
                              DeoptPoolRootKind Kind, uint64_t Token) {
  return DeoptPoolRootInput::scalar(Cell, Kind, Token);
}

const FinalDeoptPoolCurrentOccurrence *
findOccurrence(const FinalDeoptPoolBundlePlan &Plan,
               FinalDeoptPoolOccurrenceKind Kind) {
  for (const FinalDeoptPoolCurrentOccurrence &Occurrence :
       Plan.currentOccurrences())
    if (Occurrence.Kind == Kind)
      return &Occurrence;
  return nullptr;
}

} // namespace

TEST(DeoptPoolBundleLoweringTest,
     RebuildsSparseLegacyPoolAndAllDenseReferences) {
  BundleIR IR;
  SmallVector<Value *, 16> Inputs{
      IR.i32(10),
      IR.i32(10),
      IR.i64(encoding(7, jeandle::DeoptValueEncoding::ScalarValueType,
                      jeandle::T_OBJECT)),
      IR.i64(0x1000),
      IR.i32(1),
      IR.i64(encoding(8, jeandle::DeoptValueEncoding::VORefLocalType,
                      jeandle::T_OBJECT)),
      IR.i32(99),
      IR.i64(encoding(99, jeandle::DeoptValueEncoding::ScalarValueType,
                      jeandle::T_OBJECT)),
      IR.i64(0x2000),
      IR.i32(1),
      IR.i64(
          encoding(12, jeandle::DeoptValueEncoding::LocalType, jeandle::T_INT)),
      IR.Int,
      IR.i64(encoding(7, jeandle::DeoptValueEncoding::VORefLocalType,
                      jeandle::T_OBJECT)),
      IR.i32(7)};
  CallBase *CB = IR.finish(Inputs);
  ParsedDeoptBundle Source = parse(*CB);

  const ParsedDeoptField &Ref = Source.Descriptors[0].Fields[0];
  const ParsedDeoptField &Scalar = Source.Descriptors[1].Fields[0];
  const ParsedDeoptScopeValue &Root = Source.Scopes[0].Locals[0];
  DeoptPoolPlannerInput PlannerInput;
  PlannerInput.LegacyNodes.push_back(legacyNode(
      7, 0x1000, {refField(Ref.ValueCell.OperandIndex, Ref.Offset, 99)}));
  PlannerInput.LegacyNodes.push_back(
      legacyNode(99, 0x2000,
                 {scalarField(Scalar.ValueCell.OperandIndex, Scalar.Offset,
                              jeandle::T_INT, 700)}));
  PlannerInput.Roots.push_back(
      legacyRoot(Root.ValueCell.OperandIndex, DeoptPoolRootKind::Local, 7));
  FinalDeoptPoolGraphPlan Graph = plan(PlannerInput);

  DeoptPoolScalarTokenBinding ScalarBinding{700, IR.Int};
  PrepareFinalDeoptPoolBundleResult Prepared =
      prepareFinalDeoptPoolBundlePlan(Source, Graph, {ScalarBinding}, {}, *CB);
  ASSERT_TRUE(Prepared.Plan.has_value());
  EXPECT_FALSE(Prepared.Error.has_value());

  SerializeFinalDeoptPoolBundleResult Serialized =
      serializeFinalDeoptPoolBundlePlan(*Prepared.Plan, *CB);
  ASSERT_TRUE(Serialized.Inputs.has_value());
  DeoptBundleParseResult Reparsed = parseDeoptBundleInputs(*Serialized.Inputs);
  ASSERT_TRUE(Reparsed.Bundle.has_value());
  ASSERT_EQ(Reparsed.Bundle->Descriptors.size(), 2u);
  EXPECT_EQ(Reparsed.Bundle->Descriptors[0].WireID, 0);
  EXPECT_EQ(Reparsed.Bundle->Descriptors[1].WireID, 1);
  ASSERT_TRUE(
      Reparsed.Bundle->Descriptors[0].Fields[0].TargetWireID.has_value());
  EXPECT_EQ(*Reparsed.Bundle->Descriptors[0].Fields[0].TargetWireID, 1);
  ASSERT_TRUE(Reparsed.Bundle->Scopes[0].Locals[0].TargetWireID.has_value());
  EXPECT_EQ(*Reparsed.Bundle->Scopes[0].Locals[0].TargetWireID, 0);
  EXPECT_EQ(
      (*Serialized.Inputs)
          [Reparsed.Bundle->Descriptors[1].Fields[0].ValueCell.OperandIndex],
      IR.Int);
}

TEST(DeoptPoolBundleLoweringTest, RewritesOnlyTheExactCurrentScopeOccurrence) {
  BundleIR IR;
  SmallVector<Value *, 6> Inputs{
      IR.i32(20),
      IR.i32(20),
      IR.i64(encoding(0, jeandle::DeoptValueEncoding::LocalType,
                      jeandle::T_OBJECT)),
      IR.Object,
      IR.i64(encoding(1, jeandle::DeoptValueEncoding::LocalType,
                      jeandle::T_OBJECT)),
      IR.Object};
  CallBase *CB = IR.finish(Inputs);
  ParsedDeoptBundle Source = parse(*CB);
  const ParsedDeoptScopeValue &Untouched = Source.Scopes[0].Locals[0];
  const ParsedDeoptScopeValue &Root = Source.Scopes[0].Locals[1];

  DeoptPoolPlannerInput PlannerInput;
  PlannerInput.CurrentNodes.push_back(currentNode(
      55, 0x3000,
      {scalarField(InvalidDeoptPoolSemanticCellID, 8, jeandle::T_INT, 800)}));
  PlannerInput.Roots.push_back(scalarRoot(Untouched.ValueCell.OperandIndex,
                                          DeoptPoolRootKind::Local, 901));
  PlannerInput.Roots.push_back(
      scalarRoot(Root.ValueCell.OperandIndex, DeoptPoolRootKind::Local, 900));
  PlannerInput.Overlays.push_back({Root.ValueCell.OperandIndex, 55});
  FinalDeoptPoolGraphPlan Graph = plan(PlannerInput);

  DeoptPoolScalarTokenBinding ScalarBinding{800, IR.Int};
  DeoptPoolCurrentCellBinding CurrentCell{Root.ValueCell.OperandIndex,
                                          /*CurrentID=*/55};
  PrepareFinalDeoptPoolBundleResult Prepared = prepareFinalDeoptPoolBundlePlan(
      Source, Graph, {ScalarBinding}, {CurrentCell}, *CB);
  ASSERT_TRUE(Prepared.Plan.has_value());
  ASSERT_TRUE(Prepared.Plan->needsRewrite());

  const FinalDeoptPoolCurrentOccurrence *Descriptor =
      findOccurrence(*Prepared.Plan, FinalDeoptPoolOccurrenceKind::Descriptor);
  const FinalDeoptPoolCurrentOccurrence *Local =
      findOccurrence(*Prepared.Plan, FinalDeoptPoolOccurrenceKind::Local);
  ASSERT_NE(Descriptor, nullptr);
  ASSERT_NE(Local, nullptr);
  EXPECT_EQ(Descriptor->CurrentID, 55u);
  EXPECT_EQ(Local->CurrentID, 55u);
  ASSERT_TRUE(Local->SemanticCell.has_value());
  EXPECT_EQ(*Local->SemanticCell, Root.ValueCell.OperandIndex);
  EXPECT_EQ(Local->Disposition,
            FinalDeoptPoolOccurrenceDisposition::RewrittenToVORef);
  EXPECT_TRUE(
      Prepared.Plan->coversExactOccurrence(Root.ValueCell.OperandIndex, 55));

  SerializeFinalDeoptPoolBundleResult Serialized =
      serializeFinalDeoptPoolBundlePlan(*Prepared.Plan, *CB);
  ASSERT_TRUE(Serialized.Inputs.has_value());
  DeoptBundleParseResult Reparsed = parseDeoptBundleInputs(*Serialized.Inputs);
  ASSERT_TRUE(Reparsed.Bundle.has_value());
  ASSERT_EQ(Reparsed.Bundle->Descriptors.size(), 1u);
  ASSERT_EQ(Reparsed.Bundle->Scopes[0].Locals.size(), 2u);
  EXPECT_FALSE(Reparsed.Bundle->Scopes[0].Locals[0].TargetWireID.has_value());
  EXPECT_EQ((*Serialized.Inputs)
                [Reparsed.Bundle->Scopes[0].Locals[0].ValueCell.OperandIndex],
            IR.Object);
  ASSERT_TRUE(Reparsed.Bundle->Scopes[0].Locals[1].TargetWireID.has_value());
  EXPECT_EQ(*Reparsed.Bundle->Scopes[0].Locals[1].TargetWireID, 0);
}

TEST(DeoptPoolBundleLoweringTest,
     RewritesARealMonitorIntoAnEliminatedCurrentOwner) {
  BundleIR IR;
  SmallVector<Value *, 5> Inputs{
      IR.i32(30), IR.i32(30),
      IR.i64(encoding(0, jeandle::DeoptValueEncoding::MonitorType,
                      jeandle::T_OBJECT)),
      IR.Object, IR.Lock};
  CallBase *CB = IR.finish(Inputs);
  ParsedDeoptBundle Source = parse(*CB);
  const ParsedDeoptMonitor &Monitor = Source.Scopes[0].Monitors[0];

  DeoptPoolPlannerInput PlannerInput;
  PlannerInput.CurrentNodes.push_back(currentNode(77, 0x4000));
  PlannerInput.Roots.push_back(scalarRoot(
      Monitor.OwnerCell.OperandIndex, DeoptPoolRootKind::MonitorOwner, 1000));
  PlannerInput.Overlays.push_back({Monitor.OwnerCell.OperandIndex, 77});
  FinalDeoptPoolGraphPlan Graph = plan(PlannerInput);

  DeoptPoolCurrentCellBinding CurrentCell{Monitor.OwnerCell.OperandIndex,
                                          /*CurrentID=*/77};
  PrepareFinalDeoptPoolBundleResult Prepared =
      prepareFinalDeoptPoolBundlePlan(Source, Graph, {}, {CurrentCell}, *CB);
  ASSERT_TRUE(Prepared.Plan.has_value());
  const FinalDeoptPoolCurrentOccurrence *Occurrence = findOccurrence(
      *Prepared.Plan, FinalDeoptPoolOccurrenceKind::MonitorOwner);
  ASSERT_NE(Occurrence, nullptr);
  EXPECT_EQ(Occurrence->CurrentID, 77u);

  SerializeFinalDeoptPoolBundleResult Serialized =
      serializeFinalDeoptPoolBundlePlan(*Prepared.Plan, *CB);
  ASSERT_TRUE(Serialized.Inputs.has_value());
  DeoptBundleParseResult Reparsed = parseDeoptBundleInputs(*Serialized.Inputs);
  ASSERT_TRUE(Reparsed.Bundle.has_value());
  ASSERT_EQ(Reparsed.Bundle->Scopes[0].Monitors.size(), 1u);
  EXPECT_TRUE(Reparsed.Bundle->Scopes[0].Monitors[0].Eliminated);
  ASSERT_TRUE(Reparsed.Bundle->Scopes[0].Monitors[0].OwnerWireID.has_value());
  EXPECT_EQ(*Reparsed.Bundle->Scopes[0].Monitors[0].OwnerWireID, 0);
  EXPECT_EQ((*Serialized.Inputs)
                [Reparsed.Bundle->Scopes[0].Monitors[0].LockCell.OperandIndex],
            IR.Lock);
}

TEST(DeoptPoolBundleLoweringTest,
     ProducesACompleteCleanupOnlyBundleWithoutCurrentOccurrences) {
  BundleIR IR;
  SmallVector<Value *, 12> Inputs{
      IR.i32(40),
      IR.i32(40),
      IR.i64(encoding(8, jeandle::DeoptValueEncoding::ScalarValueType,
                      jeandle::T_OBJECT)),
      IR.i64(0x5000),
      IR.i32(0),
      IR.i64(encoding(9, jeandle::DeoptValueEncoding::ScalarValueType,
                      jeandle::T_OBJECT)),
      IR.i64(0x6000),
      IR.i32(0),
      IR.i64(encoding(8, jeandle::DeoptValueEncoding::VORefLocalType,
                      jeandle::T_OBJECT)),
      IR.i32(8)};
  CallBase *CB = IR.finish(Inputs);
  ParsedDeoptBundle Source = parse(*CB);
  const ParsedDeoptScopeValue &Root = Source.Scopes[0].Locals[0];

  DeoptPoolPlannerInput PlannerInput;
  PlannerInput.LegacyNodes.push_back(legacyNode(8, 0x5000, {}));
  PlannerInput.LegacyNodes.push_back(legacyNode(9, 0x6000, {}));
  PlannerInput.Roots.push_back(
      legacyRoot(Root.ValueCell.OperandIndex, DeoptPoolRootKind::Local, 8));
  FinalDeoptPoolGraphPlan Graph = plan(PlannerInput);
  ASSERT_TRUE(Graph.needsRewrite());
  ASSERT_TRUE(Graph.currentMembers().empty());

  PrepareFinalDeoptPoolBundleResult Prepared =
      prepareFinalDeoptPoolBundlePlan(Source, Graph, {}, {}, *CB);
  ASSERT_TRUE(Prepared.Plan.has_value());
  EXPECT_TRUE(Prepared.Plan->currentOccurrences().empty());
  SerializeFinalDeoptPoolBundleResult Serialized =
      serializeFinalDeoptPoolBundlePlan(*Prepared.Plan, *CB);
  ASSERT_TRUE(Serialized.Inputs.has_value());
  DeoptBundleParseResult Reparsed = parseDeoptBundleInputs(*Serialized.Inputs);
  ASSERT_TRUE(Reparsed.Bundle.has_value());
  ASSERT_EQ(Reparsed.Bundle->Descriptors.size(), 1u);
  EXPECT_EQ(Reparsed.Bundle->Descriptors[0].WireID, 0);
}

TEST(DeoptPoolBundleLoweringTest,
     KeepsAnIdempotentNoRewritePlanByteForByteSemantic) {
  BundleIR IR;
  SmallVector<Value *, 7> Inputs{
      IR.i32(50),
      IR.i32(50),
      IR.i64(encoding(0, jeandle::DeoptValueEncoding::ScalarValueType,
                      jeandle::T_OBJECT)),
      IR.i64(0x7000),
      IR.i32(0),
      IR.i64(encoding(0, jeandle::DeoptValueEncoding::VORefLocalType,
                      jeandle::T_OBJECT)),
      IR.i32(0)};
  CallBase *CB = IR.finish(Inputs);
  ParsedDeoptBundle Source = parse(*CB);
  const ParsedDeoptScopeValue &Root = Source.Scopes[0].Locals[0];

  DeoptPoolPlannerInput PlannerInput;
  PlannerInput.LegacyNodes.push_back(legacyNode(0, 0x7000, {}));
  PlannerInput.Roots.push_back(
      legacyRoot(Root.ValueCell.OperandIndex, DeoptPoolRootKind::Local, 0));
  FinalDeoptPoolGraphPlan Graph = plan(PlannerInput);
  ASSERT_FALSE(Graph.needsRewrite());

  PrepareFinalDeoptPoolBundleResult Prepared =
      prepareFinalDeoptPoolBundlePlan(Source, Graph, {}, {}, *CB);
  ASSERT_TRUE(Prepared.Plan.has_value());
  EXPECT_FALSE(Prepared.Plan->needsRewrite());
  SerializeFinalDeoptPoolBundleResult Serialized =
      serializeFinalDeoptPoolBundlePlan(*Prepared.Plan, *CB);
  ASSERT_TRUE(Serialized.Inputs.has_value());
  ASSERT_EQ(Serialized.Inputs->size(), Inputs.size());
  for (unsigned I = 0; I < Inputs.size(); ++I)
    EXPECT_EQ((*Serialized.Inputs)[I], Inputs[I]);
}

TEST(DeoptPoolBundleLoweringTest, EmitsOneWirePairForEachLongAndDoubleField) {
  BundleIR IR;
  SmallVector<Value *, 4> Inputs{
      IR.i32(60), IR.i32(60),
      IR.i64(encoding(0, jeandle::DeoptValueEncoding::LocalType,
                      jeandle::T_OBJECT)),
      IR.Object};
  CallBase *CB = IR.finish(Inputs);
  ParsedDeoptBundle Source = parse(*CB);
  const ParsedDeoptScopeValue &Root = Source.Scopes[0].Locals[0];

  DeoptPoolPlannerInput PlannerInput;
  PlannerInput.CurrentNodes.push_back(currentNode(
      88, 0x8000,
      {scalarField(InvalidDeoptPoolSemanticCellID, 8, jeandle::T_LONG, 1100),
       scalarField(InvalidDeoptPoolSemanticCellID, 16, jeandle::T_DOUBLE,
                   1200)}));
  PlannerInput.Roots.push_back(
      scalarRoot(Root.ValueCell.OperandIndex, DeoptPoolRootKind::Local, 1300));
  PlannerInput.Overlays.push_back({Root.ValueCell.OperandIndex, 88});
  FinalDeoptPoolGraphPlan Graph = plan(PlannerInput);

  SmallVector<DeoptPoolScalarTokenBinding, 2> Bindings{{1100, IR.Long},
                                                       {1200, IR.Double}};
  DeoptPoolCurrentCellBinding CurrentCell{Root.ValueCell.OperandIndex,
                                          /*CurrentID=*/88};
  PrepareFinalDeoptPoolBundleResult Prepared = prepareFinalDeoptPoolBundlePlan(
      Source, Graph, Bindings, {CurrentCell}, *CB);
  ASSERT_TRUE(Prepared.Plan.has_value());
  SerializeFinalDeoptPoolBundleResult Serialized =
      serializeFinalDeoptPoolBundlePlan(*Prepared.Plan, *CB);
  ASSERT_TRUE(Serialized.Inputs.has_value());
  DeoptBundleParseResult Reparsed = parseDeoptBundleInputs(*Serialized.Inputs);
  ASSERT_TRUE(Reparsed.Bundle.has_value());
  ASSERT_EQ(Reparsed.Bundle->Descriptors.size(), 1u);
  ASSERT_EQ(Reparsed.Bundle->Descriptors[0].Fields.size(), 2u);
  EXPECT_EQ(Reparsed.Bundle->Descriptors[0].Fields[0].Encoding.BasicType,
            jeandle::T_LONG);
  EXPECT_EQ(Reparsed.Bundle->Descriptors[0].Fields[1].Encoding.BasicType,
            jeandle::T_DOUBLE);
  EXPECT_EQ(Serialized.Inputs->size(), 11u);
}

TEST(DeoptPoolBundleLoweringTest, TrackedSourceValuesFollowRAUW) {
  BundleIR IR;
  SmallVector<Value *, 4> Inputs{
      IR.i32(70), IR.i32(70),
      IR.i64(encoding(0, jeandle::DeoptValueEncoding::LocalType,
                      jeandle::T_OBJECT)),
      IR.Object};
  CallBase *CB = IR.finish(Inputs);
  ParsedDeoptBundle Source = parse(*CB);

  DeoptPoolPlannerInput PlannerInput;
  PlannerInput.Roots.push_back(
      scalarRoot(Source.Scopes[0].Locals[0].ValueCell.OperandIndex,
                 DeoptPoolRootKind::Local, 1400));
  FinalDeoptPoolGraphPlan Graph = plan(PlannerInput);
  PrepareFinalDeoptPoolBundleResult Prepared =
      prepareFinalDeoptPoolBundlePlan(Source, Graph, {}, {}, *CB);
  ASSERT_TRUE(Prepared.Plan.has_value());

  IR.Object->replaceAllUsesWith(IR.OtherObject);
  SerializeFinalDeoptPoolBundleResult Serialized =
      serializeFinalDeoptPoolBundlePlan(*Prepared.Plan, *CB);
  ASSERT_TRUE(Serialized.Inputs.has_value());
  EXPECT_EQ(Serialized.Inputs->back(), IR.OtherObject);
}

TEST(DeoptPoolBundleLoweringTest,
     RejectsAStaleSameTypeOperandReplacementWithoutRAUW) {
  BundleIR IR;
  SmallVector<Value *, 4> Inputs{
      IR.i32(80), IR.i32(80),
      IR.i64(encoding(0, jeandle::DeoptValueEncoding::LocalType,
                      jeandle::T_OBJECT)),
      IR.Object};
  CallBase *CB = IR.finish(Inputs);
  ParsedDeoptBundle Source = parse(*CB);

  DeoptPoolPlannerInput PlannerInput;
  PlannerInput.Roots.push_back(
      scalarRoot(Source.Scopes[0].Locals[0].ValueCell.OperandIndex,
                 DeoptPoolRootKind::Local, 1500));
  FinalDeoptPoolGraphPlan Graph = plan(PlannerInput);
  PrepareFinalDeoptPoolBundleResult Prepared =
      prepareFinalDeoptPoolBundlePlan(Source, Graph, {}, {}, *CB);
  ASSERT_TRUE(Prepared.Plan.has_value());

  unsigned BundleOperand = CB->getBundleOperandsStartIndex() +
                           Source.Scopes[0].Locals[0].ValueCell.OperandIndex;
  CB->setOperand(BundleOperand, IR.OtherObject);
  SerializeFinalDeoptPoolBundleResult Serialized =
      serializeFinalDeoptPoolBundlePlan(*Prepared.Plan, *CB);
  EXPECT_FALSE(Serialized.Inputs.has_value());
  ASSERT_TRUE(Serialized.Error.has_value());
  EXPECT_EQ(Serialized.Error->Code,
            FinalDeoptPoolBundleErrorCode::StaleSourceBundle);
}

TEST(DeoptPoolBundleLoweringTest,
     RejectsWrongSemanticCellRolesAndMissingScalarTokens) {
  BundleIR IR;
  SmallVector<Value *, 4> Inputs{
      IR.i32(90), IR.i32(90),
      IR.i64(encoding(0, jeandle::DeoptValueEncoding::LocalType,
                      jeandle::T_OBJECT)),
      IR.Object};
  CallBase *CB = IR.finish(Inputs);
  ParsedDeoptBundle Source = parse(*CB);

  {
    DeoptPoolPlannerInput PlannerInput;
    PlannerInput.CurrentNodes.push_back(currentNode(99, 0x9000));
    PlannerInput.Roots.push_back(
        scalarRoot(Source.Scopes[0].FirstBCICell.OperandIndex,
                   DeoptPoolRootKind::Local, 1600));
    PlannerInput.Overlays.push_back(
        {Source.Scopes[0].FirstBCICell.OperandIndex, 99});
    FinalDeoptPoolGraphPlan Graph = plan(PlannerInput);
    DeoptPoolCurrentCellBinding CurrentCell{
        Source.Scopes[0].FirstBCICell.OperandIndex, /*CurrentID=*/99};
    PrepareFinalDeoptPoolBundleResult Prepared =
        prepareFinalDeoptPoolBundlePlan(Source, Graph, {}, {CurrentCell}, *CB);
    EXPECT_FALSE(Prepared.Plan.has_value());
    ASSERT_TRUE(Prepared.Error.has_value());
    EXPECT_EQ(Prepared.Error->Code,
              FinalDeoptPoolBundleErrorCode::UnexpectedSemanticCellRole);
  }

  {
    DeoptPoolPlannerInput PlannerInput;
    const ParsedDeoptScopeValue &Root = Source.Scopes[0].Locals[0];
    PlannerInput.CurrentNodes.push_back(
        currentNode(100, 0xa000,
                    {scalarField(InvalidDeoptPoolSemanticCellID, 8,
                                 jeandle::T_INT, 1700)}));
    PlannerInput.Roots.push_back(scalarRoot(Root.ValueCell.OperandIndex,
                                            DeoptPoolRootKind::Local, 1800));
    PlannerInput.Overlays.push_back({Root.ValueCell.OperandIndex, 100});
    FinalDeoptPoolGraphPlan Graph = plan(PlannerInput);
    DeoptPoolCurrentCellBinding CurrentCell{Root.ValueCell.OperandIndex,
                                            /*CurrentID=*/100};
    PrepareFinalDeoptPoolBundleResult Prepared =
        prepareFinalDeoptPoolBundlePlan(Source, Graph, {}, {CurrentCell}, *CB);
    EXPECT_FALSE(Prepared.Plan.has_value());
    ASSERT_TRUE(Prepared.Error.has_value());
    EXPECT_EQ(Prepared.Error->Code,
              FinalDeoptPoolBundleErrorCode::MissingScalarToken);
  }
}

TEST(DeoptPoolBundleLoweringTest, RejectsANonObjectCurrentOverlay) {
  BundleIR IR;
  SmallVector<Value *, 4> Inputs{
      IR.i32(95), IR.i32(95),
      IR.i64(
          encoding(0, jeandle::DeoptValueEncoding::LocalType, jeandle::T_INT)),
      IR.Int};
  CallBase *CB = IR.finish(Inputs);
  ParsedDeoptBundle Source = parse(*CB);
  const ParsedDeoptScopeValue &Root = Source.Scopes[0].Locals[0];

  DeoptPoolPlannerInput PlannerInput;
  PlannerInput.CurrentNodes.push_back(currentNode(101, 0xa100));
  PlannerInput.Roots.push_back(
      scalarRoot(Root.ValueCell.OperandIndex, DeoptPoolRootKind::Local, 1810));
  PlannerInput.Overlays.push_back({Root.ValueCell.OperandIndex, 101});
  FinalDeoptPoolGraphPlan Graph = plan(PlannerInput);

  DeoptPoolCurrentCellBinding CurrentCell{Root.ValueCell.OperandIndex,
                                          /*CurrentID=*/101};
  PrepareFinalDeoptPoolBundleResult Prepared =
      prepareFinalDeoptPoolBundlePlan(Source, Graph, {}, {CurrentCell}, *CB);
  EXPECT_FALSE(Prepared.Plan.has_value());
  ASSERT_TRUE(Prepared.Error.has_value());
  EXPECT_EQ(Prepared.Error->Code,
            FinalDeoptPoolBundleErrorCode::CurrentOccurrenceNotCovered);
}

TEST(DeoptPoolBundleLoweringTest,
     CoversCurrentCellsRemovedWithAnUnreachableLegacyDescriptor) {
  BundleIR IR;
  SmallVector<Value *, 14> Inputs{
      IR.i32(100),
      IR.i32(100),
      IR.i64(encoding(5, jeandle::DeoptValueEncoding::ScalarValueType,
                      jeandle::T_OBJECT)),
      IR.i64(0xb000),
      IR.i32(1),
      IR.i64(encoding(8, jeandle::DeoptValueEncoding::LocalType,
                      jeandle::T_OBJECT)),
      IR.Object,
      IR.i64(encoding(6, jeandle::DeoptValueEncoding::ScalarValueType,
                      jeandle::T_OBJECT)),
      IR.i64(0xc000),
      IR.i32(0),
      IR.i64(encoding(6, jeandle::DeoptValueEncoding::VORefLocalType,
                      jeandle::T_OBJECT)),
      IR.i32(6)};
  CallBase *CB = IR.finish(Inputs);
  ParsedDeoptBundle Source = parse(*CB);
  const ParsedDeoptField &PrunedField = Source.Descriptors[0].Fields[0];
  const ParsedDeoptScopeValue &Root = Source.Scopes[0].Locals[0];

  DeoptPoolPlannerInput PlannerInput;
  PlannerInput.LegacyNodes.push_back(
      legacyNode(5, 0xb000,
                 {scalarField(PrunedField.ValueCell.OperandIndex,
                              PrunedField.Offset, jeandle::T_OBJECT, 1900)}));
  PlannerInput.LegacyNodes.push_back(legacyNode(6, 0xc000, {}));
  PlannerInput.CurrentNodes.push_back(currentNode(123, 0xd000));
  PlannerInput.Roots.push_back(
      legacyRoot(Root.ValueCell.OperandIndex, DeoptPoolRootKind::Local, 6));
  PlannerInput.Overlays.push_back(
      {PrunedField.ValueCell.OperandIndex, /*CurrentTarget=*/123});
  FinalDeoptPoolGraphPlan Graph = plan(PlannerInput);
  ASSERT_TRUE(Graph.currentMembers().empty());

  DeoptPoolCurrentCellBinding CurrentCell{PrunedField.ValueCell.OperandIndex,
                                          /*CurrentID=*/123};
  PrepareFinalDeoptPoolBundleResult Prepared =
      prepareFinalDeoptPoolBundlePlan(Source, Graph, {}, {CurrentCell}, *CB);
  ASSERT_TRUE(Prepared.Plan.has_value());
  EXPECT_TRUE(Prepared.Plan->coversExactOccurrence(
      PrunedField.ValueCell.OperandIndex, 123));

  const FinalDeoptPoolCurrentOccurrence *Removed = nullptr;
  for (const FinalDeoptPoolCurrentOccurrence &Occurrence :
       Prepared.Plan->currentOccurrences())
    if (Occurrence.SemanticCell == PrunedField.ValueCell.OperandIndex)
      Removed = &Occurrence;
  ASSERT_NE(Removed, nullptr);
  EXPECT_EQ(Removed->Disposition,
            FinalDeoptPoolOccurrenceDisposition::RemovedByPruning);
  EXPECT_FALSE(Removed->OutputEncodingTokenIndex.has_value());
  EXPECT_FALSE(Removed->OutputValueTokenIndex.has_value());

  SerializeFinalDeoptPoolBundleResult Serialized =
      serializeFinalDeoptPoolBundlePlan(*Prepared.Plan, *CB);
  ASSERT_TRUE(Serialized.Inputs.has_value());
  DeoptBundleParseResult Reparsed = parseDeoptBundleInputs(*Serialized.Inputs);
  ASSERT_TRUE(Reparsed.Bundle.has_value());
  ASSERT_EQ(Reparsed.Bundle->Descriptors.size(), 1u);
  EXPECT_EQ(Reparsed.Bundle->Descriptors[0].Klass, 0xc000u);
}
