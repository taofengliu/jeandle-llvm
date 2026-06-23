//===- PartialEscape.h - PEA shared data structures -------------*- C++ -*-===//
//
// Copyright (c) 2026, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Shared data structures used by both PartialEscapeAnalysis and
// PartialEscapeTransform. Light header: forward-declares LLVM IR
// classes and only includes ADT containers. Method bodies are in
// llvm/lib/Analysis/Jeandle/PartialEscape.cpp.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_JEANDLE_PARTIALESCAPE_H
#define LLVM_ANALYSIS_JEANDLE_PARTIALESCAPE_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/ValueHandle.h"

#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace llvm {

class BasicBlock;
class CallBase;
class Constant;
class DataLayout;
class GetElementPtrInst;
class Instruction;
class InvokeInst;
class LLVMContext;
class PHINode;
class Type;
class Value;

namespace jeandle {

class AliasMap;
class PEABlockState;

// ===========================================================================
// 1.1  ObjectID and VirtualObject
// ===========================================================================

using ObjectID = unsigned;
static constexpr ObjectID InvalidObjectID = ~0u;

// Identity of a virtual monitorenter, carried per element of an
// ObjectState's live lock stack. EnterCall is the original
// jeandle.monitorenter call site (the effects-side anchor used when
// un-eliding the call on materialisation); BytecodeDepth is the
// Java-bytecode-level monitor depth at the enter site, taken from the
// `!jeandle.lock_depth` metadata when the JDK frontend supplies it, or
// falling back to the analyzer's monotonic NextLockEnterOrder proxy in
// lit tests that omit the metadata (then an Analyzer-run-monotonic id,
// NOT the true bytecode depth). The analyzer-side mirror struct LockEnter
// (PartialEscapeAnalysis.cpp) additionally carries an Order tag for
// loop-fixpoint convergence checks, which compare CallBase identity only
// since Order is refreshed on every re-push.
struct MonitorIdRef {
  CallBase *EnterCall;
  uint32_t BytecodeDepth;

  bool operator==(const MonitorIdRef &O) const {
    return EnterCall == O.EnterCall && BytecodeDepth == O.BytecodeDepth;
  }
  bool operator!=(const MonitorIdRef &O) const { return !(*this == O); }
};

class VirtualObject {
public:
  enum ClassKind : uint8_t { Instance, Array };

  struct FieldDesc {
    int64_t Offset;
    Type *LLVMType;
    uint8_t ByteSize;
    bool IsReference;

    bool overlaps(int64_t Off, uint8_t Size) const {
      return Off < Offset + ByteSize && Offset < Off + Size;
    }
  };

private:
  const ObjectID ID;
  ClassKind Kind;

public:
  CallBase *AllocationCall = nullptr;

  uintptr_t Klass = 0;
  uint32_t SizeInBytes = 0;

  uint32_t ArrayLength = 0;
  Type *ArrayElementType = nullptr;
  uint32_t ArrayIndexScale = 0;
  uint32_t ArrayBaseOffset = 0;

  SmallVector<FieldDesc, 8> Fields;

  // A "synthetic" VirtualObject is created at a multi-pred merge by the
  // analyzer's processBlockPhis when every incoming resolves to a DIFFERENT
  // but COMPATIBLE virtual object (same Klass / kind / array dimensions /
  // lock state, and the per-pred allocations have no other observable
  // identity). Compatibility does NOT compare per-instance field count:
  // Fields is lazily populated (path-dependent), so its size is not a
  // structural invariant; the synthetic VO's Fields is the UNION of the
  // per-pred Fields. There is no backing allocation — AllocationCall is
  // non-null (cloned from the first per-pred VO) but MUST NOT be erased or
  // used as a Materialize effect target. SyntheticSourceIDs holds the
  // per-pred VOs in PHI-incoming order; SyntheticPhi is the merge-block
  // PHINode the VO is aliased to. A later attempt to materialize a synthetic
  // VO conservatively marks it and every per-pred source VO ineligible so
  // the original allocations and stores survive.
  // TODO(cascade-materialize): see PartialEscapeAnalysis.cpp materializeAt().
  bool IsSynthetic = false;
  SmallVector<ObjectID, 4> SyntheticSourceIDs;
  PHINode *SyntheticPhi = nullptr;

  VirtualObject(ObjectID id, ClassKind k, CallBase *alloc)
      : ID(id), Kind(k), AllocationCall(alloc) {}

  ObjectID getID() const { return ID; }
  ClassKind getKind() const { return Kind; }
  bool isInstance() const { return Kind == Instance; }
  bool isArray() const { return Kind == Array; }

  int getOrCreateFieldIndex(int64_t Offset, Type *Ty);

  // Result of matching a GEP against the array's element-address pattern.
  // Index is the (possibly symbolic) Value* that names the Java-level
  // element index; ElementType is the per-element LLVM type. Callers must
  // inspect Index: a ConstantInt yields a constant element index (combine
  // it with ArrayBaseOffset + scale for the canonical byte offset); a
  // symbolic index forces the caller to bail (materialize the array).
  struct ArrayElementGEPMatch {
    llvm::Value *Index;
    llvm::Type *ElementType;
  };
  std::optional<ArrayElementGEPMatch>
  matchArrayElementGEP(GetElementPtrInst *GEP,
                       const llvm::DataLayout &DL) const;

  std::unique_ptr<VirtualObject> duplicate() const;
};

// ===========================================================================
// 1.2  FieldValue
// ===========================================================================

class FieldValue {
public:
  enum Tag : uint8_t { Unknown, Scalar, VirtualRef, MaterializedRef };

private:
  Tag T = Unknown;
  union U {
    Value *V;
    ObjectID Ref;
    U() : V(nullptr) {}
  } U;
  Type *DeclaredType = nullptr;

public:
  FieldValue() = default;

  static FieldValue unknown() { return {}; }

  static FieldValue scalar(Value *V);
  static FieldValue virtualRef(ObjectID ID, Type *RefTy);
  static FieldValue materializedRef(Value *Ptr);

  bool isUnknown() const { return T == Unknown; }
  bool isScalar() const { return T == Scalar; }
  bool isVirtualRef() const { return T == VirtualRef; }
  bool isMaterializedRef() const { return T == MaterializedRef; }

  Value *getScalar() const { assert(isScalar()); return U.V; }
  ObjectID getVirtualRef() const { assert(isVirtualRef()); return U.Ref; }
  Value *getMaterialized() const { assert(isMaterializedRef()); return U.V; }
  Type *getDeclaredType() const { return DeclaredType; }

  static Constant *defaultFor(Type *FieldType);

  bool shallowEquals(const FieldValue &O) const;
};

// ===========================================================================
// 1.3  ObjectState
// ===========================================================================

class ObjectState {
public:
  enum StateKind : uint8_t { Virtual, Materialized };

private:
  StateKind Kind = Virtual;
  SmallVector<FieldValue, 8> Entries;
  // Per-VO live monitor stack: each element is a MonitorIdRef identifying
  // the (enter-call, bytecode-depth) pair pushed by a folded monitorenter
  // on this VO whose matching monitorexit hasn't yet been seen on this
  // path. The EnterCall + BytecodeDepth fields are needed for un-elision
  // of the original calls on materialisation, the narrow cascade rule, and
  // merge-time stack-identity comparisons. The analyzer keeps a parallel
  // LiveLockEnters DenseMap keyed by ObjectID; this on-VO copy mirrors the
  // per-VO truth and is kept in lockstep by foldMonitorEnter /
  // foldMonitorExit.
  SmallVector<MonitorIdRef, 2> Locks;
  Value *MaterializedValue = nullptr;
  // CopyOnWrite is logically a sharing annotation, not part of the object's
  // observable state, so it may be set on a const-borrowed ObjectState by
  // PEABlockState's copy-on-write machinery (markAllSlotsShared).
  mutable bool CopyOnWrite = false;

public:
  explicit ObjectState(unsigned numEntries)
      : Entries(numEntries, FieldValue::unknown()) {}

  // Custom copy/move so the per-slot CopyOnWrite annotation is reset on
  // clone — a freshly-constructed copy is by definition unshared.
  ObjectState(const ObjectState &Other)
      : Kind(Other.Kind), Entries(Other.Entries), Locks(Other.Locks),
        MaterializedValue(Other.MaterializedValue), CopyOnWrite(false) {}
  ObjectState &operator=(const ObjectState &Other) {
    if (this == &Other)
      return *this;
    Kind = Other.Kind;
    Entries = Other.Entries;
    Locks = Other.Locks;
    MaterializedValue = Other.MaterializedValue;
    CopyOnWrite = false;
    return *this;
  }
  ObjectState(ObjectState &&) = default;
  ObjectState &operator=(ObjectState &&) = default;
  ~ObjectState() = default;

  StateKind getKind() const { return Kind; }
  bool isVirtual() const { return Kind == Virtual; }
  bool isMaterialized() const { return Kind == Materialized; }

  Value *getMaterializedValue() const {
    assert(isMaterialized() && MaterializedValue);
    return MaterializedValue;
  }
  bool hasLocks() const { return !Locks.empty(); }
  // Element-wise lock-stack comparison used by the depth-aware merge-time
  // stack-identity check (mergeStates) and the pre-cascade in
  // foldMonitorEnter. Compares both EnterCall and BytecodeDepth. When
  // !jeandle.lock_depth metadata is absent, BytecodeDepth holds the
  // Analyzer's NextLockEnterOrder proxy, which is stable within a single
  // processBlock walk but refreshed on every loop-fixpoint re-push — which
  // is why the analyzer-side BlockExitData snapshot identity compares only
  // EnterCall (see blockExitInfoEquivalent).
  bool locksEqual(const ObjectState &Other) const {
    if (Locks.size() != Other.Locks.size())
      return false;
    for (size_t I = 0, E = Locks.size(); I < E; ++I)
      if (Locks[I] != Other.Locks[I])
        return false;
    return true;
  }

  void materialize(Value *Ptr) {
    assert(isVirtual());
    assert(Ptr);
    Kind = Materialized;
    MaterializedValue = Ptr;
    Entries.clear();
    // A materialized object has no virtual lock state — every
    // outstanding monitorenter must have been un-elided by the caller
    // (materializeAt's lock un-elide block) before flipping the state.
    // Clear defensively so any stale element does not survive into the
    // Materialized state and confuse a later equivalentTo / hash.
    Locks.clear();
  }

  void addLock(MonitorIdRef M) {
    assert(isVirtual());
    Locks.push_back(M);
  }
  void removeLock() {
    assert(isVirtual() && !Locks.empty());
    Locks.pop_back();
  }
  // Clear the entire lock stack — used by materializeAt after un-eliding all
  // pending enters so commit()'s "non-empty Locks -> ineligible" gate doesn't
  // re-disqualify the VO.
  void clearLocks() {
    assert(isVirtual());
    Locks.clear();
  }

  void markShared() const { CopyOnWrite = true; }
  bool isShared() const { return CopyOnWrite; }

  ObjectState clone() const;
};

// ===========================================================================
// 1.4  PEABlockState
// ===========================================================================

class PEABlockState {
public:
  // Refcount shared by every PEABlockState that holds the same ObjectStates
  // vector: Count == 1 means sole owner (mutate in place); Count > 1 means
  // shared (a mutator must clone first). ObjectStates and ArrayRefCount are
  // always cloned/shared in lockstep — every code path that touches one MUST
  // touch the other.
  struct RefCount {
    mutable unsigned Count = 1;
  };

private:
  std::shared_ptr<SmallVector<std::optional<ObjectState>, 8>> ObjectStates;
  std::shared_ptr<RefCount> ArrayRefCount;

  bool Dead = false;

public:
  PEABlockState();
  PEABlockState(const PEABlockState &Other);
  PEABlockState &operator=(const PEABlockState &Other);
  // Move ctor: default is correct — moved-from Other has nullified
  // shared_ptrs, so the array's logical Count is unchanged (Other leaves,
  // *this joins, net 0). Move assignment is custom because *this had an
  // old ArrayRefCount that must be logically decremented before being
  // overwritten — otherwise the survivor of the old shared array would see
  // a stale Count and trigger needless COW clones.
  PEABlockState(PEABlockState &&) = default;
  PEABlockState &operator=(PEABlockState &&Other) noexcept;
  // Custom destructor: decrement the logical Count of our shared array so
  // any survivor sees an accurate sharer count. Without this, the shared_ptr
  // payload would be freed only when the last shared_ptr ref drops, but the
  // Count field tracked inside it would remain stale (>1) for the surviving
  // sharer, defeating the COW optimization.
  ~PEABlockState();

  void addObject(ObjectID ID, ObjectState State);

  bool hasObjectState(ObjectID ID) const;
  const ObjectState &getObjectState(ObjectID ID) const;
  ObjectState &getObjectStateForModification(ObjectID ID);
  const ObjectState *getObjectStateOptional(ObjectID ID) const;

  unsigned getStateCount() const {
    return ObjectStates ? static_cast<unsigned>(ObjectStates->size()) : 0u;
  }

  std::optional<ObjectID> resolveVirtualRef(Value *V,
                                            const AliasMap &Aliases) const;

  bool isDead() const { return Dead; }
  void markDead() { Dead = true; }

private:
  SmallVector<std::optional<ObjectState>, 8> *getArrayForModification();
};

// ===========================================================================
// 1.5  AliasMap
// ===========================================================================

class AliasMap {
  DenseMap<Value *, ObjectID> VirtualAliases;
  DenseMap<Value *, Value *> ScalarAliases;
  DenseSet<Instruction *> HasVirtualInputs;

public:
  void addVirtualAlias(Value *V, ObjectID ID);
  void addScalarAlias(Value *V, Value *Replacement);
  void resetAlias(Value *V);

  std::optional<ObjectID> getVirtualAlias(Value *V) const;
  Value *getScalarAlias(Value *V) const;

  bool hasVirtualInputs(Instruction *I) const {
    return HasVirtualInputs.count(I);
  }

  // Read-only view over the virtual alias map. Used to iterate every Value*
  // that represents a given ObjectID for the per-object eligibility cleanup
  // pass.
  const DenseMap<Value *, ObjectID> &virtualAliasesView() const {
    return VirtualAliases;
  }

  void clear();

  AliasMap snapshot() const;
  void restore(const AliasMap &S);
};

// ===========================================================================
// 1.6  PEAResult
// ===========================================================================

class PEAResult {
public:
  SmallVector<std::unique_ptr<VirtualObject>, 8> VirtualObjects;

  enum class EscapeKind : uint8_t {
    NeverEscapes,
    PartiallyEscapes,
    AlwaysEscapes
  };
  DenseMap<ObjectID, EscapeKind> EscapeClassification;

  enum class EffectKind : uint16_t {
    ReplaceLoad,
    ReplaceCall,
    ReplaceInput,
    EliminateStore,
    EliminateAllocation,
    Materialize,
    CreatePHI,
  };

  struct Effect {
    // Per-offset snapshot of a virtual object's field values at a
    // materialization point.  Used exclusively by Materialize effects.
    struct FieldEntry {
      int64_t Offset;
      FieldValue Value;
    };

    EffectKind Kind;
    BasicBlock *Block = nullptr;
    uint32_t SeqNo = 0;

    Instruction *Target = nullptr;
    // WeakTrackingVH so that erasing the insertion-point instruction (e.g. a
    // lower-SeqNo effect on the same instruction running first within a block)
    // auto-nulls the handle. applyMaterialize then recomputes a fresh IP at
    // the head of the alloc's normal-dest block, avoiding a use-after-free in
    // splitBasicBlock.
    WeakTrackingVH InsertBefore;

    Value *Replacement = nullptr;
    unsigned InputIndex = 0;
    ObjectID ObjID = InvalidObjectID;
    Type *PHIType = nullptr;
    SmallVector<Value *, 4> PHIIncomingValues;
    SmallVector<BasicBlock *, 4> PHIIncomingBlocks;

    // The unparented PHINode created by the analyzer for a CreatePHI effect.
    // The transform inserts it into Block and adds incomings from
    // PHIIncomingValues / PHIIncomingBlocks. Owned by PEAResult::OwnedPhis
    // until insertion; once inserted, the parent BasicBlock owns it via its
    // ilist and the destructor skips it.
    PHINode *PhiInst = nullptr;

    // Per-offset field-value snapshot for Materialize effects.
    SmallVector<FieldEntry, 8> FieldEntries;

    // CallBase whose "deopt" operand bundle should be copied onto the
    // materialization invoke. Set by the analyzer in materializeAt: the
    // escape-point instruction if it carries a deopt bundle, otherwise the
    // original allocation invoke.
    Instruction *DeoptBundleSource = nullptr;

    // Materialize-effect flag: true when this effect is one of multiple
    // per-pred materializations for the same OrigAlloc. The transform must
    // NOT do a global RAUW here — it would inject this pred's NewInv into
    // uses on OTHER preds, breaking SSA. Pre-merge per-pred uses (un-elided
    // enters) are rewired via ReplaceInput effects (MatPerBlock lookup);
    // post-merge uses are rewired by the matching CreatePHI effect.
    bool IsPerPred = false;

    // CreatePHI-effect flag: when true, after wiring the PHI's incomings,
    // RAUW every remaining use of the VO's OrigAlloc onto the PHI. Handles
    // the post-merge uses that the IsPerPred Materialize effects left
    // untouched; the final RAUW drives them onto the PHI.
    bool RAUWOrigToPHI = false;
  };

  DenseMap<BasicBlock *, SmallVector<Effect, 16>> BlockEffects;

  int VirtualizationDelta = 0;
  int AllocationDelta = 0;
  uint32_t NextSeqNo = 0;

  // PHI nodes created (unparented) by the analyzer for CreatePHI effects.
  // Stored as WeakTrackingVH so we can safely check for nullness in the
  // destructor — once a PHI is inserted into a BasicBlock and later RAUW'd
  // or erased by the transform's dead-code sweep, the handle auto-nulls and
  // the destructor skips it. Any PHI that's still alive AND still unparented
  // (i.e. the analyzer ran without a paired transform) is `delete`d.
  SmallVector<WeakTrackingVH, 4> OwnedPhis;

  // Non-PHI Instructions created (unparented) by the analyzer for
  // ReplaceLoad effects (e.g. a `bitcast` synthesized by `coerceToType` to
  // bridge a same-bit-width type mismatch between a stored Scalar and the
  // load's type). Same ownership rules as OwnedPhis: the transform splices
  // the instruction in before its Target; the destructor deletes any
  // handle still non-null and unparented at teardown. WeakTrackingVH guards
  // against a UAF if the inserted coercion is later erased by the dead-code
  // sweep.
  SmallVector<WeakTrackingVH, 4> OwnedInsts;

  // PHI nodes synthesized by mergeStates at a LOOP HEADER block, stored
  // separately from OwnedPhis because they must survive rollback within the
  // loop-fixpoint iteration. Each iteration re-runs mergeStates(Header) and
  // would otherwise allocate a fresh PHI per (ID, offset), producing fresh
  // Value* pointers in FieldStates so the convergence check could never
  // compare equal. The analyzer's LoopFieldPhiCache (keyed on
  // (Header, ID, offset)) returns the same PHI across iterations; only the
  // per-iteration CreatePHI Effect is rebuilt. Lifecycle is identical to
  // OwnedPhis.
  SmallVector<WeakTrackingVH, 4> OwnedLoopFieldPhis;

  // Parented LLVM PHIs the transform should RAUW to poison + erase after
  // the main Pass-2 EliminateAllocation sweep. These are Case-B aliases on
  // a virtual that ended up NeverEscapes: the PHI was registered as an
  // alias so downstream loads/stores fold through the alias map, but the
  // PHI itself is now dead (every incoming was the VO's OrigAlloc, which
  // Pass-2 RAUW'd to poison). WeakTrackingVH so a parallel delete path
  // (e.g. unreachable-block pruning) leaves a null handle the transform
  // safely skips.
  SmallVector<WeakTrackingVH, 4> CaseBAliasedPhisToErase;

  PEAResult() = default;
  PEAResult(const PEAResult &) = delete;
  PEAResult &operator=(const PEAResult &) = delete;
  PEAResult(PEAResult &&) = default;
  PEAResult &operator=(PEAResult &&) = default;
  ~PEAResult();

  ObjectID createVirtualObject(std::unique_ptr<VirtualObject> VO);

  uint32_t nextSeqNo() { return NextSeqNo++; }

  void addBlockEffect(Effect E);

  bool hasOptimizationOpportunity() const;
};

} // namespace jeandle
} // namespace llvm

#endif // LLVM_ANALYSIS_JEANDLE_PARTIALESCAPE_H
