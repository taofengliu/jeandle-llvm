//===- DeoptPoolPlanner.h - Semantic deopt object-pool planning -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines a pure graph planner for Jeandle deoptimization object
// pools. It composes the durable legacy descriptor graph with current PEA
// nodes, prunes unreachable nodes, and derives fresh dense wire IDs.
//
// The output is deliberately a semantic graph plan, not the transform-ready
// operand-bundle plan. A later lowering layer must combine it with the parsed
// bundle and produce the complete immutable output-token template held by the
// atomic deopt-pool effect.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_JEANDLE_DEOPTPOOLPLANNER_H
#define LLVM_ANALYSIS_JEANDLE_DEOPTPOOLPLANNER_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Jeandle/Deoptimization.h"
#include "llvm/Support/Compiler.h"

#include <cstdint>
#include <limits>
#include <optional>

namespace llvm {
namespace jeandle {
namespace pea {

using CurrentDeoptNodeID = uint32_t;
using DeoptPoolSemanticCellID = uint32_t;

inline constexpr CurrentDeoptNodeID InvalidCurrentDeoptNodeID =
    std::numeric_limits<CurrentDeoptNodeID>::max();
inline constexpr DeoptPoolSemanticCellID InvalidDeoptPoolSemanticCellID =
    std::numeric_limits<DeoptPoolSemanticCellID>::max();
inline constexpr uint32_t InvalidDeoptPoolWireID =
    std::numeric_limits<uint32_t>::max();

enum class DeoptPoolNodeNamespace : uint8_t { Legacy, Current };

struct DeoptPoolNodeRef {
  DeoptPoolNodeNamespace Namespace = DeoptPoolNodeNamespace::Legacy;
  uint32_t ID = 0;

  static DeoptPoolNodeRef legacy(uint32_t WireID) {
    return {DeoptPoolNodeNamespace::Legacy, WireID};
  }
  static DeoptPoolNodeRef current(CurrentDeoptNodeID ID) {
    return {DeoptPoolNodeNamespace::Current, ID};
  }

  bool operator==(const DeoptPoolNodeRef &Other) const {
    return Namespace == Other.Namespace && ID == Other.ID;
  }
};

// SemanticCell identifies the exact value cell in the parsed input bundle.
// It is opaque to the graph planner; the codec/lowering layer may use the
// bundle-relative operand index. Current fields are newly emitted cells and
// therefore use InvalidDeoptPoolSemanticCellID.
//
// ScalarToken is equally opaque. It is a planner-only handle for scalar
// payload identity; lowering must resolve it to its tracked LLVM Value. The
// numeric token itself is never serialized onto the deopt wire.
struct DeoptPoolFieldInput {
  DeoptPoolSemanticCellID SemanticCell = InvalidDeoptPoolSemanticCellID;
  int64_t Offset = 0;
  HotspotBasicType BasicType = T_ILLEGAL;
  bool IsReference = false;
  uint64_t ScalarToken = 0;
  DeoptPoolNodeRef Target;

  static DeoptPoolFieldInput scalar(DeoptPoolSemanticCellID SemanticCell,
                                    int64_t Offset, HotspotBasicType BasicType,
                                    uint64_t ScalarToken) {
    DeoptPoolFieldInput Field;
    Field.SemanticCell = SemanticCell;
    Field.Offset = Offset;
    Field.BasicType = BasicType;
    Field.ScalarToken = ScalarToken;
    return Field;
  }

  static DeoptPoolFieldInput reference(DeoptPoolSemanticCellID SemanticCell,
                                       int64_t Offset,
                                       DeoptPoolNodeRef Target) {
    DeoptPoolFieldInput Field;
    Field.SemanticCell = SemanticCell;
    Field.Offset = Offset;
    Field.BasicType = T_OBJECT;
    Field.IsReference = true;
    Field.Target = Target;
    return Field;
  }

  bool isReference() const { return IsReference; }
};

struct LegacyDeoptPoolNode {
  uint32_t WireID = InvalidDeoptPoolWireID;
  uint64_t Klass = 0;
  bool IsArray = false;
  SmallVector<DeoptPoolFieldInput, 8> Fields;
};

// CurrentNodes must be supplied in deterministic semantic discovery order.
// ID is analysis-local identity only; the planner never copies it to WireID.
struct CurrentDeoptPoolNode {
  CurrentDeoptNodeID ID = InvalidCurrentDeoptNodeID;
  uint64_t Klass = 0;
  bool IsArray = false;
  bool Describable = true;
  SmallVector<DeoptPoolFieldInput, 8> Fields;
};

enum class DeoptPoolRootKind : uint8_t { Local, Stack, MonitorOwner };

// A root cell may already be a legacy/current VORef, or it may still be a
// scalar oop cell which an exact-cell overlay reclassifies as a current VO.
struct DeoptPoolRootInput {
  DeoptPoolSemanticCellID SemanticCell = InvalidDeoptPoolSemanticCellID;
  DeoptPoolRootKind Kind = DeoptPoolRootKind::Local;
  bool IsReference = false;
  uint64_t ScalarToken = 0;
  DeoptPoolNodeRef Target;

  static DeoptPoolRootInput scalar(DeoptPoolSemanticCellID SemanticCell,
                                   DeoptPoolRootKind Kind,
                                   uint64_t ScalarToken) {
    DeoptPoolRootInput Root;
    Root.SemanticCell = SemanticCell;
    Root.Kind = Kind;
    Root.ScalarToken = ScalarToken;
    return Root;
  }

  static DeoptPoolRootInput reference(DeoptPoolSemanticCellID SemanticCell,
                                      DeoptPoolRootKind Kind,
                                      DeoptPoolNodeRef Target) {
    DeoptPoolRootInput Root;
    Root.SemanticCell = SemanticCell;
    Root.Kind = Kind;
    Root.IsReference = true;
    Root.Target = Target;
    return Root;
  }

  bool isReference() const { return IsReference; }
};

struct DeoptPoolScalarOverlay {
  DeoptPoolSemanticCellID SemanticCell = InvalidDeoptPoolSemanticCellID;
  CurrentDeoptNodeID CurrentTarget = InvalidCurrentDeoptNodeID;
};

struct DeoptPoolPlannerInput {
  SmallVector<LegacyDeoptPoolNode, 8> LegacyNodes;
  SmallVector<CurrentDeoptPoolNode, 8> CurrentNodes;
  SmallVector<DeoptPoolRootInput, 8> Roots;
  SmallVector<DeoptPoolScalarOverlay, 8> Overlays;
};

enum class DeoptPoolNodeOrigin : uint8_t { Legacy, Current };

struct FinalDeoptPoolField {
  DeoptPoolSemanticCellID SemanticCell = InvalidDeoptPoolSemanticCellID;
  int64_t Offset = 0;
  HotspotBasicType BasicType = T_ILLEGAL;
  bool IsReference = false;
  uint64_t ScalarToken = 0;
  uint32_t TargetWireID = InvalidDeoptPoolWireID;

  bool isReference() const { return IsReference; }
};

struct FinalDeoptPoolNode {
  uint32_t WireID = InvalidDeoptPoolWireID;
  uint64_t Klass = 0;
  bool IsArray = false;
  DeoptPoolNodeOrigin Origin = DeoptPoolNodeOrigin::Legacy;
  unsigned LegacySourceIndex = std::numeric_limits<unsigned>::max();
  CurrentDeoptNodeID CurrentID = InvalidCurrentDeoptNodeID;
  SmallVector<FinalDeoptPoolField, 8> Fields;
};

struct FinalDeoptPoolRoot {
  DeoptPoolSemanticCellID SemanticCell = InvalidDeoptPoolSemanticCellID;
  DeoptPoolRootKind Kind = DeoptPoolRootKind::Local;
  uint32_t TargetWireID = InvalidDeoptPoolWireID;
};

class FinalDeoptPoolGraphPlan {
  friend struct DeoptPoolPlannerAccess;

  SmallVector<FinalDeoptPoolNode, 8> Nodes;
  SmallVector<FinalDeoptPoolRoot, 8> Roots;
  SmallVector<CurrentDeoptNodeID, 8> CurrentMembers;
  bool NeedsRewrite = false;

public:
  FinalDeoptPoolGraphPlan(const FinalDeoptPoolGraphPlan &) = default;
  FinalDeoptPoolGraphPlan(FinalDeoptPoolGraphPlan &&) = default;
  FinalDeoptPoolGraphPlan &operator=(const FinalDeoptPoolGraphPlan &) = default;
  FinalDeoptPoolGraphPlan &operator=(FinalDeoptPoolGraphPlan &&) = default;

  ArrayRef<FinalDeoptPoolNode> nodes() const { return Nodes; }
  ArrayRef<FinalDeoptPoolRoot> roots() const { return Roots; }
  ArrayRef<CurrentDeoptNodeID> currentMembers() const { return CurrentMembers; }
  bool needsRewrite() const { return NeedsRewrite; }

private:
  FinalDeoptPoolGraphPlan() = default;
};

enum class DeoptPoolPlannerErrorCode : uint8_t {
  DuplicateLegacyWireID,
  DuplicateCurrentNodeID,
  DuplicateSemanticCellID,
  InvalidSemanticCellID,
  MissingNodeReference,
  InvalidScalarOverlay,
  CurrentFieldHasSemanticCell,
};

struct DeoptPoolPlannerError {
  DeoptPoolPlannerErrorCode Code;
  uint32_t Subject = 0;
};

struct DeoptPoolPlannerResult {
  std::optional<FinalDeoptPoolGraphPlan> Plan;
  SmallVector<CurrentDeoptNodeID, 4> FallbackSeeds;
  std::optional<DeoptPoolPlannerError> Error;
};

LLVM_ABI DeoptPoolPlannerResult
planDeoptPool(const DeoptPoolPlannerInput &Input);

} // namespace pea
} // namespace jeandle
} // namespace llvm

#endif // LLVM_ANALYSIS_JEANDLE_DEOPTPOOLPLANNER_H
