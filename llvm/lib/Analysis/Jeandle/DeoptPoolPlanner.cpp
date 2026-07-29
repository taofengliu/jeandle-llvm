//===- DeoptPoolPlanner.cpp - Semantic deopt object-pool planning ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/Jeandle/DeoptPoolPlanner.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallBitVector.h"

using namespace llvm;
using namespace llvm::jeandle;
using namespace llvm::jeandle::pea;

namespace llvm::jeandle::pea {

struct DeoptPoolPlannerAccess {
  static FinalDeoptPoolGraphPlan
  create(SmallVector<FinalDeoptPoolNode, 8> Nodes,
         SmallVector<FinalDeoptPoolRoot, 8> Roots,
         SmallVector<CurrentDeoptNodeID, 8> CurrentMembers, bool NeedsRewrite) {
    FinalDeoptPoolGraphPlan Plan;
    Plan.Nodes = std::move(Nodes);
    Plan.Roots = std::move(Roots);
    Plan.CurrentMembers = std::move(CurrentMembers);
    Plan.NeedsRewrite = NeedsRewrite;
    return Plan;
  }
};

} // namespace llvm::jeandle::pea

namespace {

struct SemanticCellInfo {
  bool CanOverlay;
};

DeoptPoolPlannerResult error(DeoptPoolPlannerErrorCode Code, uint32_t Subject) {
  DeoptPoolPlannerResult Result;
  Result.Error = DeoptPoolPlannerError{Code, Subject};
  return Result;
}

bool hasNode(const DeoptPoolNodeRef &Ref,
             const DenseMap<uint32_t, unsigned> &LegacyByWire,
             const DenseMap<CurrentDeoptNodeID, unsigned> &CurrentByID) {
  if (Ref.Namespace == DeoptPoolNodeNamespace::Legacy)
    return LegacyByWire.count(Ref.ID);
  return CurrentByID.count(Ref.ID);
}

uint32_t finalWireID(const DeoptPoolNodeRef &Ref,
                     const DenseMap<uint32_t, unsigned> &LegacyByWire,
                     const DenseMap<CurrentDeoptNodeID, unsigned> &CurrentByID,
                     ArrayRef<uint32_t> LegacyFinalWire,
                     ArrayRef<uint32_t> CurrentFinalWire) {
  if (Ref.Namespace == DeoptPoolNodeNamespace::Legacy)
    return LegacyFinalWire[LegacyByWire.lookup(Ref.ID)];
  return CurrentFinalWire[CurrentByID.lookup(Ref.ID)];
}

DeoptPoolNodeRef effectiveFieldTarget(
    const DeoptPoolFieldInput &Field,
    const DenseMap<DeoptPoolSemanticCellID, CurrentDeoptNodeID> &Overlays) {
  auto Overlay = Overlays.find(Field.SemanticCell);
  if (Overlay != Overlays.end())
    return DeoptPoolNodeRef::current(Overlay->second);
  return Field.Target;
}

DeoptPoolNodeRef effectiveRootTarget(
    const DeoptPoolRootInput &Root,
    const DenseMap<DeoptPoolSemanticCellID, CurrentDeoptNodeID> &Overlays) {
  auto Overlay = Overlays.find(Root.SemanticCell);
  if (Overlay != Overlays.end())
    return DeoptPoolNodeRef::current(Overlay->second);
  return Root.Target;
}

bool isSemanticNoOp(
    const DeoptPoolPlannerInput &Input,
    const DenseMap<DeoptPoolSemanticCellID, CurrentDeoptNodeID> &Overlays,
    ArrayRef<FinalDeoptPoolNode> Nodes, ArrayRef<FinalDeoptPoolRoot> Roots) {
  // A stable second round consists only of the same legacy descriptors and
  // VORef roots with already-dense IDs. SemanticCell is provenance for exact
  // lowering, not wire semantics, so it is intentionally excluded.
  if (Nodes.size() != Input.LegacyNodes.size())
    return false;
  for (unsigned I = 0; I < Nodes.size(); ++I) {
    const FinalDeoptPoolNode &Final = Nodes[I];
    const LegacyDeoptPoolNode &Legacy = Input.LegacyNodes[I];
    if (Final.Origin != DeoptPoolNodeOrigin::Legacy ||
        Final.LegacySourceIndex != I || Final.WireID != Legacy.WireID ||
        Final.Klass != Legacy.Klass || Final.IsArray != Legacy.IsArray ||
        Final.Fields.size() != Legacy.Fields.size())
      return false;
    for (unsigned FieldIndex = 0; FieldIndex < Final.Fields.size();
         ++FieldIndex) {
      const FinalDeoptPoolField &FinalField = Final.Fields[FieldIndex];
      const DeoptPoolFieldInput &LegacyField = Legacy.Fields[FieldIndex];
      if (FinalField.Offset != LegacyField.Offset ||
          FinalField.BasicType != LegacyField.BasicType)
        return false;
      if (Overlays.count(LegacyField.SemanticCell))
        return false;
      if (FinalField.isReference() != LegacyField.isReference())
        return false;
      if (FinalField.isReference()) {
        if (LegacyField.Target.Namespace != DeoptPoolNodeNamespace::Legacy ||
            FinalField.TargetWireID != LegacyField.Target.ID)
          return false;
      } else if (FinalField.ScalarToken != LegacyField.ScalarToken) {
        return false;
      }
    }
  }

  unsigned FinalRootIndex = 0;
  for (const DeoptPoolRootInput &InputRoot : Input.Roots) {
    if (Overlays.count(InputRoot.SemanticCell))
      return false;
    if (!InputRoot.isReference())
      continue;
    if (FinalRootIndex == Roots.size() ||
        InputRoot.Target.Namespace != DeoptPoolNodeNamespace::Legacy ||
        Roots[FinalRootIndex].Kind != InputRoot.Kind ||
        Roots[FinalRootIndex].TargetWireID != InputRoot.Target.ID)
      return false;
    ++FinalRootIndex;
  }
  return FinalRootIndex == Roots.size();
}

} // namespace

DeoptPoolPlannerResult
llvm::jeandle::pea::planDeoptPool(const DeoptPoolPlannerInput &Input) {
  DenseMap<uint32_t, unsigned> LegacyByWire;
  for (unsigned I = 0; I < Input.LegacyNodes.size(); ++I) {
    uint32_t WireID = Input.LegacyNodes[I].WireID;
    if (!LegacyByWire.try_emplace(WireID, I).second)
      return error(DeoptPoolPlannerErrorCode::DuplicateLegacyWireID, WireID);
  }

  DenseMap<CurrentDeoptNodeID, unsigned> CurrentByID;
  for (unsigned I = 0; I < Input.CurrentNodes.size(); ++I) {
    CurrentDeoptNodeID ID = Input.CurrentNodes[I].ID;
    if (!CurrentByID.try_emplace(ID, I).second)
      return error(DeoptPoolPlannerErrorCode::DuplicateCurrentNodeID, ID);
  }

  // Validate the complete input before doing reachability. In particular, a
  // malformed unreachable node is still rejected rather than being hidden by
  // pruning.
  DenseMap<DeoptPoolSemanticCellID, SemanticCellInfo> SemanticCells;
  auto AddSemanticCell = [&](DeoptPoolSemanticCellID Cell,
                             bool CanOverlay) -> DeoptPoolPlannerResult {
    if (Cell == InvalidDeoptPoolSemanticCellID)
      return error(DeoptPoolPlannerErrorCode::InvalidSemanticCellID, Cell);
    if (!SemanticCells.try_emplace(Cell, SemanticCellInfo{CanOverlay}).second)
      return error(DeoptPoolPlannerErrorCode::DuplicateSemanticCellID, Cell);
    return {};
  };
  auto ValidateRef =
      [&](const DeoptPoolNodeRef &Ref) -> DeoptPoolPlannerResult {
    if (!hasNode(Ref, LegacyByWire, CurrentByID))
      return error(DeoptPoolPlannerErrorCode::MissingNodeReference, Ref.ID);
    return {};
  };

  for (const LegacyDeoptPoolNode &Node : Input.LegacyNodes)
    for (const DeoptPoolFieldInput &Field : Node.Fields) {
      DeoptPoolPlannerResult CellResult =
          AddSemanticCell(Field.SemanticCell,
                          !Field.isReference() && Field.BasicType == T_OBJECT);
      if (CellResult.Error)
        return CellResult;
      if (Field.isReference()) {
        DeoptPoolPlannerResult RefResult = ValidateRef(Field.Target);
        if (RefResult.Error)
          return RefResult;
      }
    }

  for (const CurrentDeoptPoolNode &Node : Input.CurrentNodes)
    for (const DeoptPoolFieldInput &Field : Node.Fields) {
      if (Field.SemanticCell != InvalidDeoptPoolSemanticCellID)
        return error(DeoptPoolPlannerErrorCode::CurrentFieldHasSemanticCell,
                     Field.SemanticCell);
      if (Field.isReference()) {
        DeoptPoolPlannerResult RefResult = ValidateRef(Field.Target);
        if (RefResult.Error)
          return RefResult;
      }
    }

  for (const DeoptPoolRootInput &Root : Input.Roots) {
    DeoptPoolPlannerResult CellResult =
        AddSemanticCell(Root.SemanticCell, !Root.isReference());
    if (CellResult.Error)
      return CellResult;
    if (Root.isReference()) {
      DeoptPoolPlannerResult RefResult = ValidateRef(Root.Target);
      if (RefResult.Error)
        return RefResult;
    }
  }

  DenseMap<DeoptPoolSemanticCellID, CurrentDeoptNodeID> Overlays;
  for (const DeoptPoolScalarOverlay &Overlay : Input.Overlays) {
    auto Cell = SemanticCells.find(Overlay.SemanticCell);
    if (Cell == SemanticCells.end() || !Cell->second.CanOverlay ||
        !CurrentByID.count(Overlay.CurrentTarget) ||
        !Overlays.try_emplace(Overlay.SemanticCell, Overlay.CurrentTarget)
             .second)
      return error(DeoptPoolPlannerErrorCode::InvalidScalarOverlay,
                   Overlay.SemanticCell);
  }

  SmallBitVector ReachableLegacy(Input.LegacyNodes.size());
  SmallBitVector ReachableCurrent(Input.CurrentNodes.size());
  SmallVector<DeoptPoolNodeRef, 16> Worklist;
  for (const DeoptPoolRootInput &Root : Input.Roots) {
    if (Overlays.count(Root.SemanticCell) || Root.isReference())
      Worklist.push_back(effectiveRootTarget(Root, Overlays));
  }

  while (!Worklist.empty()) {
    DeoptPoolNodeRef Ref = Worklist.pop_back_val();
    if (Ref.Namespace == DeoptPoolNodeNamespace::Legacy) {
      unsigned Index = LegacyByWire.lookup(Ref.ID);
      if (ReachableLegacy.test(Index))
        continue;
      ReachableLegacy.set(Index);
      for (const DeoptPoolFieldInput &Field : Input.LegacyNodes[Index].Fields)
        if (Overlays.count(Field.SemanticCell) || Field.isReference())
          Worklist.push_back(effectiveFieldTarget(Field, Overlays));
      continue;
    }

    unsigned Index = CurrentByID.lookup(Ref.ID);
    if (ReachableCurrent.test(Index))
      continue;
    ReachableCurrent.set(Index);
    for (const DeoptPoolFieldInput &Field : Input.CurrentNodes[Index].Fields)
      if (Field.isReference())
        Worklist.push_back(Field.Target);
  }

  DeoptPoolPlannerResult Result;
  for (unsigned I = 0; I < Input.CurrentNodes.size(); ++I)
    if (ReachableCurrent.test(I) && !Input.CurrentNodes[I].Describable)
      Result.FallbackSeeds.push_back(Input.CurrentNodes[I].ID);
  if (!Result.FallbackSeeds.empty())
    return Result;

  SmallVector<uint32_t, 8> LegacyFinalWire(Input.LegacyNodes.size(),
                                           InvalidDeoptPoolWireID);
  SmallVector<uint32_t, 8> CurrentFinalWire(Input.CurrentNodes.size(),
                                            InvalidDeoptPoolWireID);
  uint32_t NextWireID = 0;
  for (unsigned I = 0; I < Input.LegacyNodes.size(); ++I)
    if (ReachableLegacy.test(I))
      LegacyFinalWire[I] = NextWireID++;
  for (unsigned I = 0; I < Input.CurrentNodes.size(); ++I)
    if (ReachableCurrent.test(I))
      CurrentFinalWire[I] = NextWireID++;

  auto BuildField = [&](const DeoptPoolFieldInput &InputField,
                        bool AllowOverlay) -> FinalDeoptPoolField {
    FinalDeoptPoolField Field;
    Field.SemanticCell = InputField.SemanticCell;
    Field.Offset = InputField.Offset;
    Field.BasicType = InputField.BasicType;
    bool HasOverlay = AllowOverlay && Overlays.count(InputField.SemanticCell);
    if (HasOverlay || InputField.isReference()) {
      DeoptPoolNodeRef Target =
          HasOverlay ? DeoptPoolNodeRef::current(
                           Overlays.lookup(InputField.SemanticCell))
                     : InputField.Target;
      Field.IsReference = true;
      Field.BasicType = T_OBJECT;
      Field.TargetWireID = finalWireID(Target, LegacyByWire, CurrentByID,
                                       LegacyFinalWire, CurrentFinalWire);
    } else {
      Field.ScalarToken = InputField.ScalarToken;
    }
    return Field;
  };

  SmallVector<FinalDeoptPoolNode, 8> FinalNodes;
  for (unsigned I = 0; I < Input.LegacyNodes.size(); ++I) {
    if (!ReachableLegacy.test(I))
      continue;
    const LegacyDeoptPoolNode &InputNode = Input.LegacyNodes[I];
    FinalDeoptPoolNode Node;
    Node.WireID = LegacyFinalWire[I];
    Node.Klass = InputNode.Klass;
    Node.IsArray = InputNode.IsArray;
    Node.Origin = DeoptPoolNodeOrigin::Legacy;
    Node.LegacySourceIndex = I;
    for (const DeoptPoolFieldInput &Field : InputNode.Fields)
      Node.Fields.push_back(BuildField(Field, /*AllowOverlay=*/true));
    FinalNodes.push_back(std::move(Node));
  }

  SmallVector<CurrentDeoptNodeID, 8> CurrentMembers;
  for (unsigned I = 0; I < Input.CurrentNodes.size(); ++I) {
    if (!ReachableCurrent.test(I))
      continue;
    const CurrentDeoptPoolNode &InputNode = Input.CurrentNodes[I];
    FinalDeoptPoolNode Node;
    Node.WireID = CurrentFinalWire[I];
    Node.Klass = InputNode.Klass;
    Node.IsArray = InputNode.IsArray;
    Node.Origin = DeoptPoolNodeOrigin::Current;
    Node.CurrentID = InputNode.ID;
    for (const DeoptPoolFieldInput &Field : InputNode.Fields)
      Node.Fields.push_back(BuildField(Field, /*AllowOverlay=*/false));
    FinalNodes.push_back(std::move(Node));
    CurrentMembers.push_back(InputNode.ID);
  }

  SmallVector<FinalDeoptPoolRoot, 8> FinalRoots;
  for (const DeoptPoolRootInput &InputRoot : Input.Roots) {
    if (!Overlays.count(InputRoot.SemanticCell) && !InputRoot.isReference())
      continue;
    DeoptPoolNodeRef Target = effectiveRootTarget(InputRoot, Overlays);
    FinalRoots.push_back({InputRoot.SemanticCell, InputRoot.Kind,
                          finalWireID(Target, LegacyByWire, CurrentByID,
                                      LegacyFinalWire, CurrentFinalWire)});
  }

  bool NeedsRewrite = !isSemanticNoOp(Input, Overlays, FinalNodes, FinalRoots);
  Result.Plan = DeoptPoolPlannerAccess::create(
      std::move(FinalNodes), std::move(FinalRoots), std::move(CurrentMembers),
      NeedsRewrite);
  return Result;
}
