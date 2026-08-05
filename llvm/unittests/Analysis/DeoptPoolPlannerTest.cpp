//===- DeoptPoolPlannerTest.cpp - Deopt pool graph planner tests ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/Jeandle/DeoptPoolPlanner.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::jeandle::pea;

namespace {

DeoptPoolFieldInput
scalarField(int64_t Offset, uint64_t Token,
            jeandle::HotspotBasicType BasicType = jeandle::T_INT,
            DeoptPoolSemanticCellID Cell = InvalidDeoptPoolSemanticCellID) {
  return DeoptPoolFieldInput::scalar(Cell, Offset, BasicType, Token);
}

DeoptPoolFieldInput
legacyRefField(int64_t Offset, uint32_t WireID,
               DeoptPoolSemanticCellID Cell = InvalidDeoptPoolSemanticCellID) {
  return DeoptPoolFieldInput::reference(Cell, Offset,
                                        DeoptPoolNodeRef::legacy(WireID));
}

DeoptPoolFieldInput currentRefField(int64_t Offset, CurrentDeoptNodeID ID) {
  return DeoptPoolFieldInput::reference(InvalidDeoptPoolSemanticCellID, Offset,
                                        DeoptPoolNodeRef::current(ID));
}

LegacyDeoptPoolNode
legacyNode(uint32_t WireID, std::initializer_list<DeoptPoolFieldInput> Fields,
           uint64_t Klass = 0x1000) {
  LegacyDeoptPoolNode Node;
  Node.WireID = WireID;
  Node.Klass = Klass;
  Node.Fields.append(Fields.begin(), Fields.end());
  return Node;
}

CurrentDeoptPoolNode
currentNode(CurrentDeoptNodeID ID,
            std::initializer_list<DeoptPoolFieldInput> Fields = {},
            bool Describable = true, uint64_t Klass = 0x2000) {
  CurrentDeoptPoolNode Node;
  Node.ID = ID;
  Node.Klass = Klass;
  Node.Describable = Describable;
  Node.Fields.append(Fields.begin(), Fields.end());
  return Node;
}

DeoptPoolRootInput
legacyRoot(DeoptPoolSemanticCellID Cell, uint32_t WireID,
           DeoptPoolRootKind Kind = DeoptPoolRootKind::Local) {
  return DeoptPoolRootInput::reference(Cell, Kind,
                                       DeoptPoolNodeRef::legacy(WireID));
}

DeoptPoolRootInput
currentRoot(DeoptPoolSemanticCellID Cell, CurrentDeoptNodeID ID,
            DeoptPoolRootKind Kind = DeoptPoolRootKind::Local) {
  return DeoptPoolRootInput::reference(Cell, Kind,
                                       DeoptPoolNodeRef::current(ID));
}

DeoptPoolRootInput scalarRoot(DeoptPoolSemanticCellID Cell, uint64_t Token,
                              DeoptPoolRootKind Kind) {
  return DeoptPoolRootInput::scalar(Cell, Kind, Token);
}

const FinalDeoptPoolNode &findCurrent(const FinalDeoptPoolGraphPlan &Plan,
                                      CurrentDeoptNodeID ID) {
  for (const FinalDeoptPoolNode &Node : Plan.nodes())
    if (Node.Origin == DeoptPoolNodeOrigin::Current && Node.CurrentID == ID)
      return Node;
  llvm_unreachable("current node missing from final plan");
}

DeoptPoolPlannerInput
serializedAsNextRound(const FinalDeoptPoolGraphPlan &Plan) {
  DeoptPoolPlannerInput Next;
  DeoptPoolSemanticCellID NextParsedCell = 1u << 30;
  for (const FinalDeoptPoolNode &Node : Plan.nodes()) {
    LegacyDeoptPoolNode Legacy;
    Legacy.WireID = Node.WireID;
    Legacy.Klass = Node.Klass;
    Legacy.IsArray = Node.IsArray;
    for (const FinalDeoptPoolField &Field : Node.Fields) {
      DeoptPoolSemanticCellID ParsedCell =
          Field.SemanticCell == InvalidDeoptPoolSemanticCellID
              ? NextParsedCell++
              : Field.SemanticCell;
      if (Field.isReference())
        Legacy.Fields.push_back(
            legacyRefField(Field.Offset, Field.TargetWireID, ParsedCell));
      else
        Legacy.Fields.push_back(scalarField(Field.Offset, Field.ScalarToken,
                                            Field.BasicType, ParsedCell));
    }
    Next.LegacyNodes.push_back(std::move(Legacy));
  }
  for (const FinalDeoptPoolRoot &Root : Plan.roots())
    Next.Roots.push_back(
        legacyRoot(Root.SemanticCell, Root.TargetWireID, Root.Kind));
  return Next;
}

void expectSameWirePlan(const FinalDeoptPoolGraphPlan &A,
                        const FinalDeoptPoolGraphPlan &B) {
  ASSERT_EQ(A.nodes().size(), B.nodes().size());
  ASSERT_EQ(A.roots().size(), B.roots().size());
  for (unsigned I = 0; I < A.nodes().size(); ++I) {
    const FinalDeoptPoolNode &AN = A.nodes()[I];
    const FinalDeoptPoolNode &BN = B.nodes()[I];
    EXPECT_EQ(AN.WireID, BN.WireID);
    EXPECT_EQ(AN.Klass, BN.Klass);
    EXPECT_EQ(AN.IsArray, BN.IsArray);
    ASSERT_EQ(AN.Fields.size(), BN.Fields.size());
    for (unsigned F = 0; F < AN.Fields.size(); ++F) {
      EXPECT_EQ(AN.Fields[F].Offset, BN.Fields[F].Offset);
      EXPECT_EQ(AN.Fields[F].BasicType, BN.Fields[F].BasicType);
      EXPECT_EQ(AN.Fields[F].isReference(), BN.Fields[F].isReference());
      if (AN.Fields[F].isReference())
        EXPECT_EQ(AN.Fields[F].TargetWireID, BN.Fields[F].TargetWireID);
      else
        EXPECT_EQ(AN.Fields[F].ScalarToken, BN.Fields[F].ScalarToken);
    }
  }
  for (unsigned I = 0; I < A.roots().size(); ++I) {
    EXPECT_EQ(A.roots()[I].Kind, B.roots()[I].Kind);
    EXPECT_EQ(A.roots()[I].TargetWireID, B.roots()[I].TargetWireID);
  }
}

} // namespace

TEST(DeoptPoolPlannerTest, DenseRenumbersSparseLegacyWireIDs) {
  DeoptPoolPlannerInput Input;
  Input.LegacyNodes.push_back(legacyNode(17, {legacyRefField(8, 99, 100)}));
  Input.LegacyNodes.push_back(
      legacyNode(99, {scalarField(12, 42, jeandle::T_INT, 101)}));
  Input.Roots.push_back(legacyRoot(200, 17));

  DeoptPoolPlannerResult Result = planDeoptPool(Input);
  ASSERT_TRUE(Result.Plan.has_value());
  EXPECT_TRUE(Result.FallbackSeeds.empty());
  const FinalDeoptPoolGraphPlan &Plan = *Result.Plan;
  ASSERT_EQ(Plan.nodes().size(), 2u);
  EXPECT_EQ(Plan.nodes()[0].WireID, 0u);
  EXPECT_EQ(Plan.nodes()[1].WireID, 1u);
  ASSERT_TRUE(Plan.nodes()[0].Fields[0].isReference());
  EXPECT_EQ(Plan.nodes()[0].Fields[0].TargetWireID, 1u);
  ASSERT_EQ(Plan.roots().size(), 1u);
  EXPECT_EQ(Plan.roots()[0].TargetWireID, 0u);
  EXPECT_TRUE(Plan.needsRewrite());
}

TEST(DeoptPoolPlannerTest, PrunesUnreachableLegacyNodes) {
  DeoptPoolPlannerInput Input;
  Input.LegacyNodes.push_back(
      legacyNode(5, {scalarField(8, 10, jeandle::T_INT, 100)}));
  Input.LegacyNodes.push_back(
      legacyNode(6, {scalarField(8, 20, jeandle::T_INT, 101)}));
  Input.Roots.push_back(legacyRoot(200, 5));

  DeoptPoolPlannerResult Result = planDeoptPool(Input);
  ASSERT_TRUE(Result.Plan.has_value());
  ASSERT_EQ(Result.Plan->nodes().size(), 1u);
  EXPECT_EQ(Result.Plan->nodes()[0].Klass, 0x1000u);
  EXPECT_EQ(Result.Plan->nodes()[0].Fields[0].ScalarToken, 10u);
  EXPECT_TRUE(Result.Plan->needsRewrite());
}

TEST(DeoptPoolPlannerTest, PreservesLegacySelfAndForwardCycles) {
  DeoptPoolPlannerInput Input;
  Input.LegacyNodes.push_back(legacyNode(
      40, {legacyRefField(8, 40, 100), legacyRefField(16, 70, 101)}));
  Input.LegacyNodes.push_back(legacyNode(70, {legacyRefField(8, 40, 102)}));
  Input.Roots.push_back(legacyRoot(200, 40));

  DeoptPoolPlannerResult Result = planDeoptPool(Input);
  ASSERT_TRUE(Result.Plan.has_value());
  ASSERT_EQ(Result.Plan->nodes().size(), 2u);
  EXPECT_EQ(Result.Plan->nodes()[0].Fields[0].TargetWireID, 0u);
  EXPECT_EQ(Result.Plan->nodes()[0].Fields[1].TargetWireID, 1u);
  EXPECT_EQ(Result.Plan->nodes()[1].Fields[0].TargetWireID, 0u);
}

TEST(DeoptPoolPlannerTest, OverlaysOnlyTheExactLegacyScalarCell) {
  DeoptPoolPlannerInput Input;
  Input.LegacyNodes.push_back(
      legacyNode(3, {scalarField(8, 777, jeandle::T_OBJECT, 100),
                     scalarField(16, 777, jeandle::T_OBJECT, 101)}));
  Input.CurrentNodes.push_back(currentNode(91, {scalarField(8, 123)}));
  Input.Roots.push_back(legacyRoot(200, 3));
  Input.Overlays.push_back({/*SemanticCell=*/101, 91});

  DeoptPoolPlannerResult Result = planDeoptPool(Input);
  ASSERT_TRUE(Result.Plan.has_value());
  const FinalDeoptPoolGraphPlan &Plan = *Result.Plan;
  ASSERT_EQ(Plan.nodes().size(), 2u);
  EXPECT_FALSE(Plan.nodes()[0].Fields[0].isReference());
  EXPECT_EQ(Plan.nodes()[0].Fields[0].ScalarToken, 777u);
  ASSERT_TRUE(Plan.nodes()[0].Fields[1].isReference());
  EXPECT_EQ(Plan.nodes()[0].Fields[1].TargetWireID, 1u);
  EXPECT_EQ(findCurrent(Plan, 91).WireID, 1u);
}

TEST(DeoptPoolPlannerTest, SeparatesLegacyAndCurrentIdentityNamespaces) {
  DeoptPoolPlannerInput Input;
  Input.LegacyNodes.push_back(
      legacyNode(7, {scalarField(8, 1, jeandle::T_OBJECT, 100)}));
  Input.CurrentNodes.push_back(currentNode(7, {currentRefField(8, 7)}));
  Input.Roots.push_back(legacyRoot(200, 7));
  Input.Overlays.push_back({100, 7});

  DeoptPoolPlannerResult Result = planDeoptPool(Input);
  ASSERT_TRUE(Result.Plan.has_value());
  const FinalDeoptPoolGraphPlan &Plan = *Result.Plan;
  ASSERT_EQ(Plan.nodes().size(), 2u);
  EXPECT_EQ(Plan.nodes()[0].WireID, 0u);
  EXPECT_EQ(Plan.nodes()[0].Fields[0].TargetWireID, 1u);
  EXPECT_EQ(findCurrent(Plan, 7).WireID, 1u);
  EXPECT_EQ(findCurrent(Plan, 7).Fields[0].TargetWireID, 1u);
}

TEST(DeoptPoolPlannerTest, UsesSemanticInputOrderForCurrentNodes) {
  DeoptPoolPlannerInput Input;
  Input.CurrentNodes.push_back(currentNode(900));
  Input.CurrentNodes.push_back(currentNode(2));
  Input.Roots.push_back(currentRoot(200, 2));
  Input.Roots.push_back(currentRoot(201, 900));

  DeoptPoolPlannerResult First = planDeoptPool(Input);
  DeoptPoolPlannerResult Second = planDeoptPool(Input);
  ASSERT_TRUE(First.Plan.has_value());
  ASSERT_TRUE(Second.Plan.has_value());
  EXPECT_EQ(findCurrent(*First.Plan, 900).WireID, 0u);
  EXPECT_EQ(findCurrent(*First.Plan, 2).WireID, 1u);
  expectSameWirePlan(*First.Plan, *Second.Plan);
}

TEST(DeoptPoolPlannerTest, ProducesCleanupOnlyPlan) {
  DeoptPoolPlannerInput Input;
  Input.LegacyNodes.push_back(legacyNode(8, {}));
  Input.LegacyNodes.push_back(legacyNode(9, {}));
  Input.Roots.push_back(legacyRoot(200, 8));

  DeoptPoolPlannerResult Result = planDeoptPool(Input);
  ASSERT_TRUE(Result.Plan.has_value());
  EXPECT_TRUE(Result.Plan->currentMembers().empty());
  ASSERT_EQ(Result.Plan->nodes().size(), 1u);
  EXPECT_TRUE(Result.Plan->needsRewrite());
}

TEST(DeoptPoolPlannerTest, SecondRoundPlanningIsIdempotent) {
  DeoptPoolPlannerInput FirstInput;
  FirstInput.LegacyNodes.push_back(
      legacyNode(11, {scalarField(8, 1, jeandle::T_OBJECT, 100)}));
  FirstInput.CurrentNodes.push_back(currentNode(55, {scalarField(8, 9)}));
  FirstInput.Roots.push_back(legacyRoot(200, 11));
  FirstInput.Overlays.push_back({100, 55});

  DeoptPoolPlannerResult First = planDeoptPool(FirstInput);
  ASSERT_TRUE(First.Plan.has_value());
  EXPECT_TRUE(First.Plan->needsRewrite());

  DeoptPoolPlannerInput SecondInput = serializedAsNextRound(*First.Plan);
  DeoptPoolPlannerResult Second = planDeoptPool(SecondInput);
  ASSERT_TRUE(Second.Plan.has_value());
  EXPECT_FALSE(Second.Plan->needsRewrite());
  EXPECT_TRUE(Second.Plan->currentMembers().empty());
  expectSameWirePlan(*First.Plan, *Second.Plan);
}

TEST(DeoptPoolPlannerTest, ReportsOnlyReachableBadCurrentFallbackSeeds) {
  DeoptPoolPlannerInput Input;
  Input.CurrentNodes.push_back(currentNode(100, {currentRefField(8, 9)}));
  Input.CurrentNodes.push_back(currentNode(9, {}, /*Describable=*/false));
  Input.CurrentNodes.push_back(currentNode(2, {}, /*Describable=*/false));
  Input.Roots.push_back(currentRoot(200, 100));

  DeoptPoolPlannerResult Result = planDeoptPool(Input);
  EXPECT_FALSE(Result.Plan.has_value());
  ASSERT_EQ(Result.FallbackSeeds.size(), 1u);
  EXPECT_EQ(Result.FallbackSeeds[0], 9u);
  EXPECT_FALSE(Result.Error.has_value());
}

TEST(DeoptPoolPlannerTest, OverlaysScopeAndMonitorScalarCellsExactly) {
  DeoptPoolPlannerInput Input;
  Input.CurrentNodes.push_back(currentNode(10));
  Input.CurrentNodes.push_back(currentNode(20));
  Input.Roots.push_back(scalarRoot(300, 0xaaaa, DeoptPoolRootKind::Stack));
  Input.Roots.push_back(
      scalarRoot(301, 0xbbbb, DeoptPoolRootKind::MonitorOwner));
  Input.Overlays.push_back({300, 10});
  Input.Overlays.push_back({301, 20});

  DeoptPoolPlannerResult Result = planDeoptPool(Input);
  ASSERT_TRUE(Result.Plan.has_value());
  ASSERT_EQ(Result.Plan->roots().size(), 2u);
  EXPECT_EQ(Result.Plan->roots()[0].SemanticCell, 300u);
  EXPECT_EQ(Result.Plan->roots()[0].Kind, DeoptPoolRootKind::Stack);
  EXPECT_EQ(Result.Plan->roots()[0].TargetWireID,
            findCurrent(*Result.Plan, 10).WireID);
  EXPECT_EQ(Result.Plan->roots()[1].SemanticCell, 301u);
  EXPECT_EQ(Result.Plan->roots()[1].Kind, DeoptPoolRootKind::MonitorOwner);
  EXPECT_EQ(Result.Plan->roots()[1].TargetWireID,
            findCurrent(*Result.Plan, 20).WireID);
}

TEST(DeoptPoolPlannerTest, NoRootsRemovesEveryLegacyNode) {
  DeoptPoolPlannerInput Input;
  Input.LegacyNodes.push_back(legacyNode(1, {}));
  Input.LegacyNodes.push_back(legacyNode(2, {}));

  DeoptPoolPlannerResult Result = planDeoptPool(Input);
  ASSERT_TRUE(Result.Plan.has_value());
  EXPECT_TRUE(Result.Plan->nodes().empty());
  EXPECT_TRUE(Result.Plan->roots().empty());
  EXPECT_TRUE(Result.Plan->currentMembers().empty());
  EXPECT_TRUE(Result.Plan->needsRewrite());
}

TEST(DeoptPoolPlannerTest, RejectsStructurallyInvalidInput) {
  {
    DeoptPoolPlannerInput Input;
    Input.LegacyNodes.push_back(legacyNode(1, {legacyRefField(8, 99, 100)}));
    Input.Roots.push_back(legacyRoot(200, 1));
    DeoptPoolPlannerResult Result = planDeoptPool(Input);
    ASSERT_TRUE(Result.Error.has_value());
    EXPECT_EQ(Result.Error->Code,
              DeoptPoolPlannerErrorCode::MissingNodeReference);
  }
  {
    DeoptPoolPlannerInput Input;
    Input.LegacyNodes.push_back(legacyNode(1, {}));
    Input.LegacyNodes.push_back(legacyNode(1, {}));
    DeoptPoolPlannerResult Result = planDeoptPool(Input);
    ASSERT_TRUE(Result.Error.has_value());
    EXPECT_EQ(Result.Error->Code,
              DeoptPoolPlannerErrorCode::DuplicateLegacyWireID);
  }
  {
    DeoptPoolPlannerInput Input;
    Input.CurrentNodes.push_back(currentNode(7));
    Input.CurrentNodes.push_back(currentNode(7));
    DeoptPoolPlannerResult Result = planDeoptPool(Input);
    ASSERT_TRUE(Result.Error.has_value());
    EXPECT_EQ(Result.Error->Code,
              DeoptPoolPlannerErrorCode::DuplicateCurrentNodeID);
  }
  {
    DeoptPoolPlannerInput Input;
    Input.CurrentNodes.push_back(currentNode(7));
    Input.Overlays.push_back({999, 7});
    DeoptPoolPlannerResult Result = planDeoptPool(Input);
    ASSERT_TRUE(Result.Error.has_value());
    EXPECT_EQ(Result.Error->Code,
              DeoptPoolPlannerErrorCode::InvalidScalarOverlay);
  }
  {
    DeoptPoolPlannerInput Input;
    Input.LegacyNodes.push_back(
        legacyNode(1, {scalarField(8, 1, jeandle::T_INT, 100)}));
    Input.CurrentNodes.push_back(currentNode(7));
    Input.Roots.push_back(legacyRoot(200, 1));
    Input.Overlays.push_back({100, 7});
    DeoptPoolPlannerResult Result = planDeoptPool(Input);
    ASSERT_TRUE(Result.Error.has_value());
    EXPECT_EQ(Result.Error->Code,
              DeoptPoolPlannerErrorCode::InvalidScalarOverlay);
  }
  {
    DeoptPoolPlannerInput Input;
    Input.LegacyNodes.push_back(
        legacyNode(1, {scalarField(8, 1, jeandle::T_INT, 100)}));
    Input.Roots.push_back(legacyRoot(100, 1));
    DeoptPoolPlannerResult Result = planDeoptPool(Input);
    ASSERT_TRUE(Result.Error.has_value());
    EXPECT_EQ(Result.Error->Code,
              DeoptPoolPlannerErrorCode::DuplicateSemanticCellID);
  }
  {
    DeoptPoolPlannerInput Input;
    Input.LegacyNodes.push_back(legacyNode(1, {scalarField(8, 1)}));
    Input.Roots.push_back(legacyRoot(200, 1));
    DeoptPoolPlannerResult Result = planDeoptPool(Input);
    ASSERT_TRUE(Result.Error.has_value());
    EXPECT_EQ(Result.Error->Code,
              DeoptPoolPlannerErrorCode::InvalidSemanticCellID);
  }
}
