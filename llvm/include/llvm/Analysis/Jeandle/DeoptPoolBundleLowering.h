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

// Binds one planner scalar token to the SSA value it stands for. The value
// is held by a tracking handle so a legitimate RAUW is followed and a
// deletion without replacement is observed as a null.
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

// One output operand of the final bundle: either a live SSA value followed
// through a tracking handle, or a grammar constant materialized as an i32 /
// i64 ConstantInt at serialization time.
enum class FinalDeoptPoolBundleTokenKind : uint8_t {
  // A live SSA value (a scalar payload or an unchanged source operand).
  TrackedValue,
  // An i32 constant (field counts, VORef wire IDs).
  ImmediateI32,
  // An i64 constant (deopt value encodings, klass identities).
  ImmediateI64,
};

// One operand of the token template. ExpectedTrackedType snapshots the
// tracked value's type at plan time so serialization can re-validate it
// against the live IR.
class FinalDeoptPoolBundleToken {
public:
  static FinalDeoptPoolBundleToken tracked(Value *V);
  static FinalDeoptPoolBundleToken immediateI32(uint32_t Value);
  static FinalDeoptPoolBundleToken immediateI64(uint64_t Value);

  FinalDeoptPoolBundleTokenKind kind() const { return Kind; }
  Value *trackedValue() const;
  Type *expectedTrackedType() const { return ExpectedTrackedType; }
  uint32_t immediateI32Value() const {
    return static_cast<uint32_t>(Immediate);
  }
  uint64_t immediateI64Value() const { return Immediate; }

private:
  FinalDeoptPoolBundleTokenKind Kind =
      FinalDeoptPoolBundleTokenKind::ImmediateI32;
  WeakTrackingVH Tracked;
  Type *ExpectedTrackedType = nullptr;
  uint64_t Immediate = 0;
};

// Where in the bundle one occurrence of a current node lives. Exact
// occurrences carry a source cell; generated ones (a current descriptor
// header or a current node's reference field) do not.
enum class FinalDeoptPoolOccurrenceKind : uint8_t {
  // The VO descriptor header of a current node.
  Descriptor,
  // A descriptor field: exact for a legacy descriptor's cell, generated
  // for a current node's reference field.
  DescriptorField,
  // A local slot of a scope.
  Local,
  // An expression-stack slot of a scope.
  Stack,
  // A monitor owner cell.
  MonitorOwner,
};

// What the rewrite did to one occurrence.
enum class FinalDeoptPoolOccurrenceDisposition : uint8_t {
  // The cell (or the node itself) is emitted as a reference to the current
  // node's wire ID.
  RewrittenToVORef,
  // The cell belonged to a legacy descriptor pruned as unreachable; nothing
  // is emitted for it.
  RemovedByPruning,
};

// One occurrence of a current node in the final bundle. SemanticCell is set
// for exact source-cell occurrences and absent for generated ones (a current
// descriptor header or a current node's field). OutputEncodingTokenIndex /
// OutputValueTokenIndex locate the emitted tokens for RewrittenToVORef
// occurrences; a Descriptor occurrence has no value token because the header
// encoding alone identifies the node.
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

// The immutable, transform-ready bundle plan: the parsed source bundle and
// the semantic graph plan combined into one complete token template in final
// wire order, plus the classified current-node occurrences. Held by the
// atomic deopt-pool effect; the transform serializes the tokens into the
// replacement operand list after re-validating the plan against the live IR.
// Construction is restricted to the lowering builder through
// FinalDeoptPoolBundlePlanAccess so every instance is a validated plan.
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

  // Whether the exact source cell SemanticCell is an occurrence of the
  // current node CurrentID in this plan.
  bool coversExactOccurrence(DeoptPoolSemanticCellID SemanticCell,
                             CurrentDeoptNodeID CurrentID) const;

private:
  FinalDeoptPoolBundlePlan(
      ParsedDeoptBundle Source, FinalDeoptPoolGraphPlan Graph,
      SmallVector<FinalDeoptPoolBundleToken, 32> Tokens,
      SmallVector<FinalDeoptPoolCurrentOccurrence, 8> CurrentOccurrences,
      bool NeedsRewrite);

  // The parsed source bundle; OriginalInputs are tracking handles that
  // follow RAUW, so Source stays coherent with the live IR.
  ParsedDeoptBundle Source;
  // The semantic pool plan being lowered.
  FinalDeoptPoolGraphPlan Graph;
  // The complete output operands in final wire order.
  SmallVector<FinalDeoptPoolBundleToken, 32> Tokens;
  // Every current-node occurrence, exact and generated.
  SmallVector<FinalDeoptPoolCurrentOccurrence, 8> CurrentOccurrences;
  // Whether the pool differs from the original bundle at all.
  bool NeedsRewrite = false;
};

enum class FinalDeoptPoolBundleErrorCode : uint8_t {
  // The live call's bundle differs from the one the plan was built from.
  StaleSourceBundle,
  // The parsed snapshot fails its structural fingerprint check, or the
  // parsed tree does not account for every source operand exactly once.
  InvalidSourceFingerprint,
  // Two scalar-token bindings share a token.
  DuplicateScalarToken,
  // A plan field references a token with no binding.
  MissingScalarToken,
  // A tracked value was deleted without replacement.
  DeadTrackedValue,
  // A tracked value's type is incompatible with its slot's basic type.
  InvalidScalarType,
  // The graph plan is internally inconsistent (non-dense wire IDs, dangling
  // targets, or current members out of order).
  InvalidWireGraph,
  // A legacy plan node disagrees with the parsed descriptor it claims to
  // keep.
  InvalidLegacySource,
  // A field offset does not fit the signed wire encoding.
  InvalidFieldOffset,
  // Two plan or source structures claim the same semantic cell.
  DuplicateSemanticCell,
  // A cell appears with a role other than its position in the grammar.
  UnexpectedSemanticCellRole,
  // A final root and its source cell disagree on the root kind.
  RootKindMismatch,
  // The current-cell table is malformed (invalid or duplicated cell).
  DuplicateCurrentOccurrence,
  // A claimed current occurrence is inconsistent with the plan or source.
  CurrentOccurrenceNotCovered,
  // A plan cell rewritten to reference a current node has no exact-cell
  // binding.
  MissingCurrentOccurrence,
};

// A lowering failure. Subject is the offending identity (semantic cell,
// token, wire ID, or token index) when the code has one.
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

// Combine the parsed source bundle and the semantic graph plan into one
// immutable token template. ScalarTokens resolves every planner scalar
// token; CurrentCells lists every exact source cell reclassified as a
// current-node reference. Runs during analysis and performs no IR mutation.
LLVM_ABI PrepareFinalDeoptPoolBundleResult prepareFinalDeoptPoolBundlePlan(
    const ParsedDeoptBundle &Source, const FinalDeoptPoolGraphPlan &Graph,
    ArrayRef<DeoptPoolScalarTokenBinding> ScalarTokens,
    ArrayRef<DeoptPoolCurrentCellBinding> CurrentCells);

// Materialize the plan's token template as the replacement operand list for
// the deopt bundle of CurrentSite. Re-validates the plan against the live IR
// first: the bundle must still match the parsed fingerprint, and every
// tracked value must be alive with its plan-time type.
LLVM_ABI SerializeFinalDeoptPoolBundleResult serializeFinalDeoptPoolBundlePlan(
    const FinalDeoptPoolBundlePlan &Plan, const CallBase &CurrentSite);

} // namespace llvm::jeandle::pea

#endif // LLVM_ANALYSIS_JEANDLE_DEOPTPOOLBUNDLELOWERING_H
