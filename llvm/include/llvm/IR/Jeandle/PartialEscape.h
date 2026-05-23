//===-- PartialEscape.h - PEA shared data structures -----------*- C++ -*-===//
//
// Part of the Jeandle JIT compiler.
//
// Shared data structures used by both PartialEscapeAnalysis and
// PartialEscapeTransform. Light header: forward-declares LLVM IR
// classes and only includes ADT containers. Method bodies are in
// llvm/lib/Transforms/Jeandle/PartialEscape.cpp.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_IR_JEANDLE_PARTIALESCAPE_H
#define LLVM_IR_JEANDLE_PARTIALESCAPE_H

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
// ObjectState's live lock stack. The pair (EnterCall, BytecodeDepth) is the
// Jeandle analogue of Graal's LockState (jdk.graal.compiler.nodes.virtual.
// LockState): EnterCall is the original jeandle.monitorenter call site (the
// effects-side anchor used when un-eliding the call on materialisation);
// BytecodeDepth is the Java-bytecode-level monitor depth at the enter site,
// stable across re-pushes within a loop fixpoint. When the JDK frontend
// supplies a `!jeandle.lock_depth = !{i32 N}` metadata node on the call,
// foldMonitorEnter uses that value. Lit tests that omit the metadata fall
// back to the analyzer's monotonic NextLockEnterOrder proxy (so the depth is
// then an Analyzer-run-monotonic identifier, NOT the true bytecode depth).
// Cascade rules and merge-time stack-identity checks compare BytecodeDepth
// instead of the Order proxy when the metadata is available, recovering
// Graal parity. See also LockEnter (private,
// PartialEscapeAnalysis.cpp), which mirrors this struct for the analyzer-
// side LiveLockEnters DenseMap and additionally carries the legacy Order
// tag used by loop-fixpoint convergence checks (which must compare CallBase
// identity ONLY because the Order is monotonically refreshed on every
// re-push and so cannot be used for stable comparisons across iterations).
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

  static constexpr int64_t ArrayLengthSlotOffset = -1;

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

  bool IsSingleUsageAllocation = false;
  bool IdentityHashObserved = false;

  // PEA B10 (boxed-primitive tagging): JBasicType index of the boxed
  // primitive when this VirtualObject's Klass is one of the eight
  // java.lang autobox wrapper classes (Boolean=0..Double=7); otherwise
  // 9 (JBasicType::Count, the sentinel "not boxed"). Populated in
  // tier1Allocate from the IsBoxed VMCallback (when registered) — kept at
  // the sentinel for lit tests without a callback log, which leaves the
  // boxed-virtual fold path inert. Used by the icmp eq fold (B10 Phase 3)
  // to substitute structural value comparison for object-identity, and
  // by synthesizeCaseC (B10 Phase 4) to drop the identity-bail when two
  // boxed virtuals of the same primitive kind reach a merge.
  //
  // Held as a uint8_t to match the JBasicType enum's underlying type
  // without dragging the VMConstants header into PartialEscape.h. The
  // sentinel value 9 (JBasicType::Count) is the agreed "no boxing" code
  // shared with VMCallback::IsBoxed; do not use 0 (which is Boolean).
  uint8_t BoxedPrimitiveKind = 9; // JBasicType::Count sentinel
  bool isBoxedPrimitive() const { return BoxedPrimitiveKind != 9; }

  // PHI Case C synthesis. A "synthetic" VirtualObject is one created at a
  // multi-pred merge by the analyzer's processBlockPhis when every incoming
  // resolves to a DIFFERENT but COMPATIBLE virtual object (same Klass / kind
  // / entry count / lock state, and the per-pred allocations have no other
  // observable identity). There is no per-pred allocation backing this VO —
  // AllocationCall is non-null (cloned from the first per-pred VO) but MUST
  // NOT be erased or used as a Materialize effect target. SyntheticSourceIDs
  // holds the per-pred VOs in PHI-incoming order; SyntheticPhi is the LLVM
  // PHINode in the merge block that the new VO is aliased to. If the
  // analyzer later attempts to materialize a synthetic VO (e.g. a downstream
  // escape consumes the PHI), it conservatively marks both the synthetic VO
  // AND every per-pred source VO ineligible so the original allocations and
  // stores survive in IR — the cascade-materialize path is not implemented
  // (Graal's MergeProcessor.processPhi with synthesized VirtualObjectNode
  // and ensureMaterialized at each predecessor).
  bool IsSynthetic = false;
  SmallVector<ObjectID, 4> SyntheticSourceIDs;
  PHINode *SyntheticPhi = nullptr;

  VirtualObject(ObjectID id, ClassKind k, CallBase *alloc)
      : ID(id), Kind(k), AllocationCall(alloc) {}

  ObjectID getID() const { return ID; }
  ClassKind getKind() const { return Kind; }
  bool isInstance() const { return Kind == Instance; }
  bool isArray() const { return Kind == Array; }

  int getFieldIndex(int64_t Offset, uint8_t ByteSize) const;
  int getOrCreateFieldIndex(int64_t Offset, Type *Ty);
  int getArrayLengthFieldIndex() const;
  int64_t arrayElementOffset(uint32_t Index) const;

  // Result of matching a GEP against the array's element-address pattern.
  // Index is the (possibly symbolic) Value* that names the Java-level
  // element index; ElementType is the per-element LLVM type. Callers must
  // inspect Index: a ConstantInt yields a constant element index (use
  // arrayElementOffset to recover the canonical byte offset); a symbolic
  // index forces the caller to bail (materialize the array).
  struct ArrayElementGEPMatch {
    llvm::Value *Index;
    llvm::Type *ElementType;
  };
  std::optional<ArrayElementGEPMatch>
  matchArrayElementGEP(GetElementPtrInst *GEP,
                       const llvm::DataLayout &DL) const;

  unsigned entryCount() const;

  static Type *getMaterializedType(LLVMContext &Ctx);

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

  Tag getTag() const { return T; }
  bool isUnknown() const { return T == Unknown; }
  bool isScalar() const { return T == Scalar; }
  bool isVirtualRef() const { return T == VirtualRef; }
  bool isMaterializedRef() const { return T == MaterializedRef; }

  Value *getScalar() const { assert(isScalar()); return U.V; }
  ObjectID getVirtualRef() const { assert(isVirtualRef()); return U.Ref; }
  Value *getMaterialized() const { assert(isMaterializedRef()); return U.V; }
  Type *getDeclaredType() const { return DeclaredType; }

  void setDeclaredType(Type *Ty) { DeclaredType = Ty; }

  static Constant *defaultFor(Type *FieldType);

  bool shallowEquals(const FieldValue &O) const;
  llvm::hash_code hash() const;
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
  // Per-VO live monitor stack. Each element is a MonitorIdRef
  // identifying the (enter-call, bytecode-depth) pair pushed by a folded
  // monitorenter on this VO whose matching monitorexit hasn't yet been seen
  // on this path. Replaces the prior `unsigned LockCount`: the size is the
  // count, and the additional EnterCall + BytecodeDepth fields are needed
  // for (a) un-elision of the original calls on materialisation, (b) the
  // narrow cascade rule (other.front().BytecodeDepth < this.back().
  // BytecodeDepth), and (c) merge-time stack-identity comparisons. The
  // analyzer keeps a parallel DenseMap (LiveLockEnters) for run-wide
  // bookkeeping because the snapshot/restore plumbing for BlockExitData is
  // keyed by ObjectID rather than per-VO ObjectState; this on-VO copy mirrors
  // the per-VO truth for callers that already hold an ObjectState handle and
  // is kept in lockstep with the DenseMap by foldMonitorEnter / foldMonitorExit.
  SmallVector<MonitorIdRef, 2> Locks;
  Value *MaterializedValue = nullptr;
  // CopyOnWrite is logically a sharing annotation, not part of the object's
  // observable state, so it may be set on a const-borrowed ObjectState by
  // PEABlockState::adoptObjectStates.
  mutable bool CopyOnWrite = false;

public:
  explicit ObjectState(unsigned numEntries)
      : Entries(numEntries, FieldValue::unknown()) {}

  // Custom copy/move so the per-slot CopyOnWrite annotation is reset on
  // clone — a freshly-constructed copy is by definition unshared. This
  // matches Graal's `private ObjectState(ObjectState other)` (see
  // ObjectState.java:88-94), which does not copy the `copyOnWrite` field.
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

  ArrayRef<FieldValue> entries() const {
    assert(isVirtual());
    return Entries;
  }
  const FieldValue &getEntry(unsigned Idx) const {
    assert(isVirtual());
    return Entries[Idx];
  }
  unsigned getEntryCount() const {
    assert(isVirtual());
    return Entries.size();
  }
  Value *getMaterializedValue() const {
    assert(isMaterialized() && MaterializedValue);
    return MaterializedValue;
  }
  unsigned getLockCount() const { return Locks.size(); }
  bool hasLocks() const { return !Locks.empty(); }
  ArrayRef<MonitorIdRef> getLocks() const { return Locks; }
  // Element-wise lock-stack comparison used by the depth-aware merge-time
  // stack-identity check (mergeStates) and the pre-cascade in
  // foldMonitorEnter. Compares both EnterCall AND
  // BytecodeDepth, which gives Graal-parity locksEqual semantics when the
  // JDK frontend supplies !jeandle.lock_depth metadata. When the metadata
  // is absent (lit tests, JDK build that predates Phase-1 emission), the
  // BytecodeDepth field holds the Analyzer's monotonic NextLockEnterOrder
  // proxy; that proxy is stable for the duration of a single processBlock
  // walk but is refreshed on every loop-fixpoint re-push, which is why the
  // analyzer-side per-block snapshot identity (BlockExitData) intentionally
  // compares only EnterCall — see blockExitInfoEquivalent for the rationale.
  bool locksEqual(const ObjectState &Other) const {
    if (Locks.size() != Other.Locks.size())
      return false;
    for (size_t I = 0, E = Locks.size(); I < E; ++I)
      if (Locks[I] != Other.Locks[I])
        return false;
    return true;
  }

  void setEntry(unsigned Idx, FieldValue V) {
    assert(isVirtual());
    Entries[Idx] = V;
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

  void updateMaterializedValue(Value *NewPtr) {
    assert(isMaterialized() && NewPtr);
    MaterializedValue = NewPtr;
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

  llvm::hash_code hash() const;
};

// ===========================================================================
// 1.4  PEABlockState
// ===========================================================================

class PEABlockState {
public:
  // Refcount object shared by every PEABlockState that holds the same
  // ObjectStates backing vector. Logically parallel to Graal's
  // PartialEscapeBlockState.RefCount (PartialEscapeBlockState.java:77-79).
  // Count == 1 means we are the sole owner: mutations may happen in place.
  // Count > 1 means the backing is shared with at least one other block
  // state: any mutator must clone the vector first (drop our ref, point
  // at a fresh shared_ptr<vector>, fresh RefCount{1}).
  //
  // Invariant: ObjectStates and ArrayRefCount are always cloned/shared in
  // lockstep. If two PEABlockStates have the same shared_ptr<RefCount> they
  // also have the same shared_ptr<SmallVector>, and vice versa. Every code
  // path that touches one MUST touch the other.
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

  void resetObjectStates(unsigned NumObjects);

  void adoptObjectStates(const PEABlockState &Other);

  std::optional<ObjectID> resolveVirtualRef(Value *V,
                                            const AliasMap &Aliases) const;

  bool isDead() const { return Dead; }
  void markDead() { Dead = true; }

private:
  SmallVector<std::optional<ObjectState>, 8> *getArrayForModification();
  void ensureSize(unsigned Size);
};

// ===========================================================================
// 1.5  AliasMap
// ===========================================================================

class AliasMap {
  DenseMap<Value *, ObjectID> VirtualAliases;
  DenseMap<Value *, Value *> ScalarAliases;
  DenseSet<Instruction *> HasVirtualInputs;
  DenseSet<Instruction *> HasScalarReplacedInputs;

public:
  void addVirtualAlias(Value *V, ObjectID ID);
  void addScalarAlias(Value *V, Value *Replacement);
  void resetAlias(Value *V);

  std::optional<ObjectID> getVirtualAlias(Value *V) const;
  Value *getScalarAlias(Value *V) const;

  bool hasVirtualInputs(Instruction *I) const {
    return HasVirtualInputs.count(I);
  }
  bool hasScalarReplacedInputs(Instruction *I) const {
    return HasScalarReplacedInputs.count(I);
  }

  // Read-only view over the virtual alias map. Used to iterate every Value*
  // that represents a given ObjectID for the per-object eligibility cleanup
  // pass.
  const DenseMap<Value *, ObjectID> &virtualAliasesView() const {
    return VirtualAliases;
  }

  Value *resolve(Value *V, const PEABlockState &State) const;

  void clear();

  AliasMap snapshot() const;
  void restore(const AliasMap &S);

  void invalidate(Value *V);
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
    // Tracked via WeakTrackingVH so that erasing the chosen insertion-point
    // instruction (e.g. when a lower-SeqNo ReplaceLoad / ReplaceCall effect on
    // the same instruction runs first within a block) auto-nulls the handle.
    // applyMaterialize detects the null and recomputes a fresh safe IP at the
    // head of the alloc's normal-dest block, avoiding a use-after-free in
    // splitBasicBlock.
    WeakTrackingVH InsertBefore;

    Value *Replacement = nullptr;
    unsigned InputIndex = 0;
    ObjectID ObjID = InvalidObjectID;
    Type *PHIType = nullptr;
    SmallVector<Value *, 4> PHIIncomingValues;
    SmallVector<BasicBlock *, 4> PHIIncomingBlocks;

    // The unparented PHINode created by the analyzer for a CreatePHI
    // effect. The transform inserts it into Block and adds incomings using
    // PHIIncomingValues / PHIIncomingBlocks. Ownership is held by
    // PEAResult::OwnedPhis until the transform inserts it; once inserted,
    // the parent BasicBlock owns it via its ilist, and the destructor
    // skips it.
    PHINode *PhiInst = nullptr;

    // Per-offset field-value snapshot for Materialize effects.
    SmallVector<FieldEntry, 8> FieldEntries;

    // CallBase whose "deopt" operand bundle should be copied onto the
    // materialization invoke. Set by the analyzer in materializeAt: the
    // escape-point instruction if it carries a deopt bundle, otherwise the
    // original allocation invoke.
    Instruction *DeoptBundleSource = nullptr;

    // Materialize-effect flag: when true, the effect is one
    // of multiple per-pred materializations for the same OrigAlloc. The
    // transform must NOT do a global RAUW for this materialize because the
    // first-wins RAUW would inject this pred's NewInv into uses on OTHER
    // preds, breaking SSA. Pre-merge per-pred uses (un-elided enters) are
    // rewired via ReplaceInput effects with MatPerBlock lookup; post-merge
    // uses are rewired by the matching CreatePHI effect (RAUWOrigToPHI).
    bool IsPerPred = false;

    // CreatePHI-effect flag: when true, after wiring the
    // PHI's incomings (via MatPerBlock for per-pred placeholders), RAUW
    // every remaining use of the VO's OrigAlloc onto the PHI. This handles
    // the post-merge uses that the per-pred IsPerPred Materialize effects
    // intentionally left untouched. Coupled with IsPerPred Materialize:
    // applyMaterialize defers all OrigAlloc rewiring, and the CreatePHI
    // here drives the final RAUW so post-merge users see the PHI.
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
  // ReplaceLoad effects, e.g. a `bitcast` synthesized by `coerceToType` to
  // bridge a same-bit-width type mismatch between a stored Scalar and the
  // load's type. Same ownership/tracking rules as OwnedPhis: until the
  // transform's ReplaceLoad handler splices the instruction in before its
  // Target, the analyzer holds the unique owning reference; the destructor
  // deletes any handle that is still non-null and unparented at PEAResult
  // teardown. WeakTrackingVH guards against a UAF if the inserted coercion
  // is later erased by the dead-code sweep.
  SmallVector<WeakTrackingVH, 4> OwnedInsts;

  // PHI nodes synthesized by mergeStates at a LOOP HEADER block. Stored
  // separately from OwnedPhis because they must SURVIVE rollback within the
  // A1 loop-fixpoint iteration. The fixpoint may take up to 10 passes over
  // the loop body; each pass re-runs mergeStates(Header) and would otherwise
  // allocate a fresh PHI per (ID, offset) per iteration, ballooning
  // OwnedPhis and (worse) producing fresh Value* pointers in FieldStates so
  // the convergence check can never compare equal. The analyzer's
  // LoopFieldPhiCache (keyed on (Header, ID, offset)) returns the same PHI
  // across iterations; only the per-iteration CreatePHI Effect (in
  // BlockEffects[Header]) is rebuilt — its PHIIncomingValues/Blocks list
  // reflects whatever the LAST iteration's mergeStates computed. Ownership
  // and lifecycle (delete-if-unparented at PEAResult teardown) are identical
  // to OwnedPhis.
  SmallVector<WeakTrackingVH, 4> OwnedLoopFieldPhis;

  // Parented LLVM PHIs the analyzer wants the transform to RAUW to
  // poison + erase after the main Pass-2 EliminateAllocation sweep. Used
  // for Case-B aliases on a virtual that ended up NeverEscapes: the PHI
  // was registered as an alias for the VO so downstream loads/stores fold
  // through the alias map, but the PHI itself is now dead in IR (every
  // incoming was the VO's OrigAlloc, which Pass-2 just RAUW'd to poison).
  // Without an explicit erase the PHI survives as
  // `phi ptr addrspace(1) [poison, %a], [poison, %b]` until a downstream
  // InstCombine canonicalisation reaps it; in single-shot PEA tests
  // (no iterative wrapper) it survives forever. Mirrors Graal's
  // PartialEscapeClosure.deleteNode(phi) at
  // PartialEscapeClosure.java:1410-1413. WeakTrackingVH so a parallel
  // delete path (e.g. unreachable-block pruning) leaves a null
  // handle that the transform safely skips.
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

#endif // LLVM_IR_JEANDLE_PARTIALESCAPE_H
