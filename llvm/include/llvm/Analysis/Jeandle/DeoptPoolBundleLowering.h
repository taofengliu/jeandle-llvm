//===- DeoptPoolBundleLowering.h - Complete deopt bundle plans -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file lowers a semantic deopt object-pool graph into one immutable,
// complete operand-bundle token plan. It performs no IR mutation.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_JEANDLE_DEOPTPOOLBUNDLELOWERING_H
#define LLVM_ANALYSIS_JEANDLE_DEOPTPOOLBUNDLELOWERING_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/Jeandle/DeoptPoolPlanner.h"
#include "llvm/Analysis/Jeandle/PartialEscapeUtils.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/Support/Compiler.h"

#include <cstdint>
#include <optional>

namespace llvm {
class CallBase;
class Type;
class Value;
} // namespace llvm

namespace llvm::jeandle::pea {

struct DeoptPoolScalarTokenBinding {
  uint64_t Token = 0;
  WeakTrackingVH Value;
};

// The planner records final reachability, so a current overlay in a pruned
// legacy descriptor is intentionally absent from currentMembers(). Lowering
// receives this complete table to retain exact source-use coverage.
struct DeoptPoolCurrentCellBinding {
  DeoptPoolSemanticCellID SemanticCell = InvalidDeoptPoolSemanticCellID;
  CurrentDeoptNodeID CurrentID = InvalidCurrentDeoptNodeID;
};

enum class FinalDeoptPoolBundleTokenKind : uint8_t {
  TrackedValue,
  ImmediateI32,
  ImmediateI64,
};

class FinalDeoptPoolBundleToken {
public:
  static FinalDeoptPoolBundleToken
  tracked(Value *V, DeoptSemanticCellRole Role,
          std::optional<unsigned> SourceSemanticCell);
  static FinalDeoptPoolBundleToken
  immediateI32(uint32_t Value, DeoptSemanticCellRole Role,
               std::optional<unsigned> SourceSemanticCell = std::nullopt);
  static FinalDeoptPoolBundleToken
  immediateI64(uint64_t Value, DeoptSemanticCellRole Role,
               std::optional<unsigned> SourceSemanticCell = std::nullopt);

  FinalDeoptPoolBundleTokenKind kind() const { return Kind; }
  DeoptSemanticCellRole role() const { return Role; }
  std::optional<unsigned> sourceSemanticCell() const {
    return SourceSemanticCell;
  }
  Value *trackedValue() const;
  Type *expectedTrackedType() const { return ExpectedTrackedType; }
  uint32_t immediateI32Value() const {
    return static_cast<uint32_t>(Immediate);
  }
  uint64_t immediateI64Value() const { return Immediate; }

private:
  FinalDeoptPoolBundleTokenKind Kind =
      FinalDeoptPoolBundleTokenKind::ImmediateI32;
  DeoptSemanticCellRole Role = DeoptSemanticCellRole::ScopeValue;
  std::optional<unsigned> SourceSemanticCell;
  WeakTrackingVH Tracked;
  Type *ExpectedTrackedType = nullptr;
  uint64_t Immediate = 0;
};

enum class FinalDeoptPoolOccurrenceKind : uint8_t {
  Descriptor,
  DescriptorField,
  Local,
  Stack,
  MonitorOwner,
};

enum class FinalDeoptPoolOccurrenceDisposition : uint8_t {
  RewrittenToVORef,
  RemovedByPruning,
};

struct FinalDeoptPoolCurrentOccurrence {
  CurrentDeoptNodeID CurrentID = InvalidCurrentDeoptNodeID;
  FinalDeoptPoolOccurrenceKind Kind = FinalDeoptPoolOccurrenceKind::Descriptor;
  FinalDeoptPoolOccurrenceDisposition Disposition =
      FinalDeoptPoolOccurrenceDisposition::RewrittenToVORef;
  std::optional<DeoptPoolSemanticCellID> SemanticCell;
  std::optional<unsigned> OutputEncodingTokenIndex;
  std::optional<unsigned> OutputValueTokenIndex;
};

struct FinalDeoptPoolBundlePlanAccess;

class FinalDeoptPoolBundlePlan {
  friend struct FinalDeoptPoolBundlePlanAccess;

public:
  FinalDeoptPoolBundlePlan(const FinalDeoptPoolBundlePlan &) = default;
  FinalDeoptPoolBundlePlan(FinalDeoptPoolBundlePlan &&) = default;
  FinalDeoptPoolBundlePlan &
  operator=(const FinalDeoptPoolBundlePlan &) = default;
  FinalDeoptPoolBundlePlan &operator=(FinalDeoptPoolBundlePlan &&) = default;

  const ParsedDeoptBundle &source() const { return Source; }
  const FinalDeoptPoolGraphPlan &graph() const { return Graph; }
  ArrayRef<FinalDeoptPoolBundleToken> tokens() const { return Tokens; }
  ArrayRef<FinalDeoptPoolCurrentOccurrence> currentOccurrences() const {
    return CurrentOccurrences;
  }
  bool needsRewrite() const { return NeedsRewrite; }

  bool coversExactOccurrence(DeoptPoolSemanticCellID SemanticCell,
                             CurrentDeoptNodeID CurrentID) const;

private:
  FinalDeoptPoolBundlePlan(
      ParsedDeoptBundle Source, FinalDeoptPoolGraphPlan Graph,
      SmallVector<FinalDeoptPoolBundleToken, 32> Tokens,
      SmallVector<FinalDeoptPoolCurrentOccurrence, 8> CurrentOccurrences,
      bool NeedsRewrite);

  ParsedDeoptBundle Source;
  FinalDeoptPoolGraphPlan Graph;
  SmallVector<FinalDeoptPoolBundleToken, 32> Tokens;
  SmallVector<FinalDeoptPoolCurrentOccurrence, 8> CurrentOccurrences;
  bool NeedsRewrite = false;
};

enum class FinalDeoptPoolBundleErrorCode : uint8_t {
  StaleSourceBundle,
  InvalidSourceFingerprint,
  DuplicateScalarToken,
  MissingScalarToken,
  DeadTrackedValue,
  InvalidScalarType,
  InvalidWireGraph,
  InvalidLegacySource,
  InvalidFieldOffset,
  DuplicateSemanticCell,
  UnexpectedSemanticCellRole,
  RootKindMismatch,
  DuplicateCurrentOccurrence,
  CurrentOccurrenceNotCovered,
  MissingCurrentOccurrence,
};

struct FinalDeoptPoolBundleError {
  FinalDeoptPoolBundleErrorCode Code =
      FinalDeoptPoolBundleErrorCode::InvalidSourceFingerprint;
  uint64_t Subject = 0;
};

struct PrepareFinalDeoptPoolBundleResult {
  std::optional<FinalDeoptPoolBundlePlan> Plan;
  std::optional<FinalDeoptPoolBundleError> Error;
};

struct SerializeFinalDeoptPoolBundleResult {
  std::optional<SmallVector<Value *, 32>> Inputs;
  std::optional<FinalDeoptPoolBundleError> Error;
};

LLVM_ABI PrepareFinalDeoptPoolBundleResult prepareFinalDeoptPoolBundlePlan(
    const ParsedDeoptBundle &Source, const FinalDeoptPoolGraphPlan &Graph,
    ArrayRef<DeoptPoolScalarTokenBinding> ScalarTokens,
    ArrayRef<DeoptPoolCurrentCellBinding> CurrentCells,
    const CallBase &SourceSite);

LLVM_ABI SerializeFinalDeoptPoolBundleResult serializeFinalDeoptPoolBundlePlan(
    const FinalDeoptPoolBundlePlan &Plan, const CallBase &CurrentSite);

} // namespace llvm::jeandle::pea

#endif // LLVM_ANALYSIS_JEANDLE_DEOPTPOOLBUNDLELOWERING_H
