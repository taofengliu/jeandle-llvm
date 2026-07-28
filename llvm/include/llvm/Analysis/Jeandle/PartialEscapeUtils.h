//===- PartialEscapeUtils.h - PEA helpers ------------------------*- C++
//-*-===//
//
// Copyright (c) 2026, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Pure helpers used by both the analysis and the transform pass.  No state.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_JEANDLE_PARTIALESCAPEUTILS_H
#define LLVM_ANALYSIS_JEANDLE_PARTIALESCAPEUTILS_H

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/Jeandle/PartialEscape.h"
#include "llvm/IR/Jeandle/VMConstants.h"
#include "llvm/IR/Value.h"
#include <cstdint>
#include <optional>

namespace llvm {
class CallBase;
class DataLayout;
class Function;
class GetElementPtrInst;
class Instruction;
class IntrinsicInst;
class LLVMContext;
class Type;
} // namespace llvm

namespace llvm::jeandle::pea {

// Callee-name predicates. Match a CallBase by callee Function name. Tolerates
// CallInst, InvokeInst, and any subclass of CallBase. Returns false on inline
// asm, indirect calls, or NULL Function.
bool isJeandleCallNamed(const CallBase *CB, StringRef Name);
bool isJeandleNewInstance(const CallBase *CB);
bool isJeandleNewArray(const CallBase *CB);
bool isJeandleAllocation(const CallBase *CB); // either of the above
bool isJeandleArrayLength(const CallBase *CB);
bool isJeandleLoadKlass(const CallBase *CB);
bool isJeandleGetClass(const CallBase *CB);
bool isJeandleCheckCast(const CallBase *CB);
bool isJeandleInstanceOf(const CallBase *CB);
bool isJeandleCheckInstanceOf(const CallBase *CB);
bool isJeandleCheckIfValueBased(const CallBase *CB);
bool isJeandleArrayStoreCheck(const CallBase *CB);
bool isJeandlePostBarrier(const CallBase *CB);
bool isJeandleMonitorEnter(const CallBase *CB);
bool isJeandleMonitorExit(const CallBase *CB);
bool isJeandleRegisterFinalizerIfNeeded(const CallBase *CB);

// Whether processIntrinsic treats II's ordinary operands and non-deopt operand
// bundles as non-escaping. Deopt bundles remain executable frame state and
// require a surviving RewriteDeoptBundleEffect.
bool isPEAHandledNonEscapingIntrinsic(const IntrinsicInst *II);

// Extract the Java element basic type from an array klass pointer.
// TODO: returns std::nullopt when VMCallbacks are unavailable; pending
// full VMCallbacks integration.
std::optional<JBasicType> elementTypeForArrayKlass(uintptr_t ArrayKlass);

// Map a JBasicType to its LLVM IR storage type for one element (e.g., Byte→i8,
// Object→ptr addrspace(1)).  Returns nullptr if Kind == Count.
Type *llvmElementTypeFor(JBasicType Kind, LLVMContext &Ctx);

// Whether Ty may be emitted by materialization as an LLVM atomic store.
// Mirrors Verifier::visitStoreInst/checkAtomicMemAccessSize: a sized,
// fixed-width integer, pointer, floating-point, or vector thereof whose total
// width is at least one byte and a power of two.
bool isLegalMaterializationAtomicType(Type *Ty, const DataLayout &DL);

// Checked arithmetic for the signed byte-offset model shared by pointer
// resolution, field ranges, array addressing, and deopt descriptors. An empty
// result means the value cannot be represented by int64_t and callers must
// treat the access as unknown.
std::optional<int64_t> checkedOffsetAdd(int64_t LHS, int64_t RHS);
std::optional<int64_t> checkedOffsetSub(int64_t LHS, int64_t RHS);
std::optional<int64_t> checkedArrayElementOffset(int64_t Base, int64_t Index,
                                                 uint64_t Scale);

// Field state uses DenseMap<int64_t, ...>; its empty and tombstone keys cannot
// represent real offsets. Such offsets are unknown to PEA.
bool isUsableFieldOffset(int64_t Offset);

// Return whether the half-open ranges overlap. An empty result means at least
// one endpoint is not representable; it is not proof of non-overlap.
std::optional<bool> checkedRangesOverlap(int64_t AStart, uint64_t ASize,
                                         int64_t BStart, uint64_t BSize);

// Strip pointer-identity-preserving operations (bitcast, addrspacecast within
// addrspace(1), freeze, launder/strip.invariant.group, ptr.annotation, and a
// pre-existing same-width inttoptr(ptrtoint(x)) round-trip) and
// constant-offset GEPs, accumulating the constant offset into *OutOffset.
// Instruction dispatch still treats PtrToIntInst as an identity observation;
// structural round-trip support does not keep a virtual live across it. Returns
// the root pointer. Sets
// *Unresolved = true if a non-constant GEP index, a non-representable APInt, or
// an overflowing accumulated offset was encountered (then the accumulated
// offset is invalid). Shared structural helper used by resolveVirtualRef and
// resolveFieldOffset.
Value *stripPointerCastsAndOffsets(Value *Ptr, const DataLayout &DL,
                                   int64_t *OutOffset, bool *Unresolved);

// Result of resolving the virtual-object identity carried by an LLVM value.
// Poison is a refinement wildcard while resolving a PHI/select, but is never
// itself a defined identity. Undef and all other unresolved values are
// Unknown.
class VirtualIdentityResult {
public:
  enum class Kind : uint8_t { DefinedIdentity, PoisonWildcard, Unknown };

  static VirtualIdentityResult defined(ObjectID ID) {
    assert(ID != InvalidObjectID);
    return VirtualIdentityResult(Kind::DefinedIdentity, ID);
  }
  static VirtualIdentityResult poisonWildcard() {
    return VirtualIdentityResult(Kind::PoisonWildcard, InvalidObjectID);
  }
  static VirtualIdentityResult unknown() {
    return VirtualIdentityResult(Kind::Unknown, InvalidObjectID);
  }

  Kind getKind() const { return K; }
  bool isDefined() const { return K == Kind::DefinedIdentity; }
  bool isPoisonWildcard() const { return K == Kind::PoisonWildcard; }
  ObjectID getObjectID() const {
    assert(isDefined());
    return ID;
  }

private:
  VirtualIdentityResult(Kind K, ObjectID ID) : K(K), ID(ID) {}

  Kind K;
  ObjectID ID;
};

enum class VirtualIdentityMode : uint8_t {
  // Resolve the underlying virtual object. Constant-offset derived pointers
  // keep the base identity.
  BaseObject,
  // Resolve only values that denote the complete object at byte offset zero
  // on every defined path.
  WholeObject
};

// Resolve V using the shared DefinedIdentity/PoisonWildcard/Unknown model.
// Cycle-safe via a small visited set. WholeObject mode rejects non-zero and
// symbolic offsets recursively through PHI/select.
VirtualIdentityResult resolveVirtualIdentity(
    Value *V, const PEABlockState &State, const AliasMap &Aliases,
    const DataLayout &DL,
    VirtualIdentityMode Mode = VirtualIdentityMode::BaseObject);

// Compatibility wrapper for callers interested only in a defined base-object
// identity.
std::optional<ObjectID> resolveVirtualRef(Value *V, const PEABlockState &State,
                                          const AliasMap &Aliases,
                                          const DataLayout &DL);

// Whether V is provably unable to denote TargetID's virtual allocation.
// This is deliberately target-relative: an identity-Unknown PHI/select may
// still be distinct when every alternative is external, but not when any
// alternative can carry TargetID, poison, undef, a derived address, or an
// unrecognised pointer-producing structure.
bool isProvablyDistinctFromVirtual(Value *V, ObjectID TargetID,
                                   const PEABlockState &State,
                                   const AliasMap &Aliases,
                                   const DataLayout &DL);

// Resolve V to the byte offset from its (presumed virtual) base pointer.
// Returns std::nullopt if V is not a constant-offset access (e.g., GEP with
// non-constant index, inttoptr, or unrecognised pattern).
std::optional<int64_t> resolveFieldOffset(Value *Ptr, const DataLayout &DL);

// Walk Root's users through pointer-value carriers that do not themselves
// observe the pointer (GEP/casts/freeze/PHI/select and transparent pointer
// intrinsics). Return true when a semantic use is not accepted by IsRemoved.
// Analysis uses this for final NeverEscapes eligibility; the transform repeats
// it as a debug invariant against the committed effect plan.
bool hasUnremovedSemanticUses(Value *Root,
                              function_ref<bool(const Use &)> IsRemoved);

// Find the start index (into the "deopt" bundle's Inputs) of the CURRENT
// (innermost) deopt scope's duplicated-BCI pair. A Jeandle deopt bundle is
// [root scope][inlinee scope]... with the innermost (current-method) scope
// LAST, each scope opening with an adjacent pair of equal-valued i32
// constants (the duplicated BCI marker; the root scope's pair is preceded by
// an i64 should_reexecute slot, inlinee scopes by a MethodType marker pair).
// The scan runs BACKWARD over the bundle inputs so inlinee scopes (appended
// last) are found first. Returns std::nullopt for a missing bundle, a
// malformed pair (mismatched adjacent values), or no pair at all — callers
// must treat those as "scope boundaries unknown" and bail conservatively
// (NEVER report_fatal_error on arbitrary IR: PEA may run on any legal LLVM
// IR, not only frontend-produced bundles).
std::optional<unsigned> findInnermostDeoptScopeBCIPairStart(const CallBase &CB);

/// Returns the start index of the FIRST (root) scope's duplicated-BCI pair in
/// the "deopt" operand bundle of CB, or std::nullopt if the bundle is missing
/// or carries no adjacent equal-i32 pair. A bundle is laid out as
/// [root scope][inlinee scope]... and every scope begins with a duplicated
/// BCI marker (two adjacent equal i32 constants). The marker is always
/// immediately preceded by at least one i64 (the should_reexecute slot; an
/// inlinee scope additionally carries its MethodType pair), and every i32
/// slot VALUE is always followed by an i64 encoding — so an adjacent
/// equal-i32 pair can only ever be a scope's BCI marker, and the FIRST one
/// anchors the root scope. PEA places ALL VO descriptors into the root
/// scope's VO section (the deopt-point-level object pool — C2's
/// dump_object_pool-before-scope-values analog), so this finder (not the
/// innermost one) anchors the descriptor insert position. Graceful: returns
/// std::nullopt on malformed bundles so callers can bail conservatively.
std::optional<unsigned> findFirstDeoptScopeBCIPairStart(const CallBase &CB);

// Returns the klass pointer (as uintptr_t) attached to the allocation call's
// klass argument, or 0 if not statically known.  Wraps
// jeandle::extractKlassConstant for the allocation-specific operand index.
uintptr_t extractAllocationKlass(const CallBase *AllocCB);

// Returns the constant size-in-bytes operand of jeandle.new_instance.
// std::nullopt if non-constant.
std::optional<uint32_t> extractInstanceSize(const CallBase *NewInstance);

// Returns the constant length operand of jeandle.new_array.
// std::nullopt if non-constant.
std::optional<uint32_t> extractArrayLength(const CallBase *NewArray);

} // namespace llvm::jeandle::pea

#endif
