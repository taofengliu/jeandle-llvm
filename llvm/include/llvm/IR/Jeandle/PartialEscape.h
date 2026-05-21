//===-- PartialEscape.h - PEA shared data structures -----------*- C++ -*-===//
//
// Part of the Jeandle JIT compiler.
//
// Shared data structures used by both PartialEscapeAnalysis and
// PartialEscapeTransform. See PEA-Plan.txt section 1 for the design
// contract. Light header: forward-declares LLVM IR classes and only
// includes ADT containers. Method bodies are in
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

  Value *ArrayLengthVal = nullptr;
  uint32_t ArrayLength = 0;
  Type *ArrayElementType = nullptr;
  uint32_t ArrayIndexScale = 0;
  uint32_t ArrayBaseOffset = 0;

  SmallVector<FieldDesc, 8> Fields;

  bool IsSingleUsageAllocation = false;
  bool IdentityHashObserved = false;
  bool MustPreserveLocks = false;

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
  std::optional<uint32_t> matchArrayElementGEP(GetElementPtrInst *GEP) const;

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
  unsigned LockCount = 0;
  Value *MaterializedValue = nullptr;
  // CopyOnWrite is logically a sharing annotation, not part of the object's
  // observable state, so it may be set on a const-borrowed ObjectState by
  // PEABlockState::adoptObjectStates.
  mutable bool CopyOnWrite = false;

public:
  explicit ObjectState(unsigned numEntries)
      : Entries(numEntries, FieldValue::unknown()) {}

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
  unsigned getLockCount() const { return LockCount; }
  bool hasLocks() const { return LockCount != 0; }

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
  }

  void updateMaterializedValue(Value *NewPtr) {
    assert(isMaterialized() && NewPtr);
    MaterializedValue = NewPtr;
  }

  void addLock() {
    assert(isVirtual());
    ++LockCount;
  }
  void removeLock() {
    assert(isVirtual() && LockCount > 0);
    --LockCount;
  }

  void markShared() const { CopyOnWrite = true; }
  bool isShared() const { return CopyOnWrite; }

  ObjectState clone() const;

  bool equivalentTo(const ObjectState &O) const;
  llvm::hash_code hash() const;
};

// ===========================================================================
// 1.4  PEABlockState
// ===========================================================================

class PEABlockState {
public:
  struct RefCount {
    mutable unsigned Count = 1;
  };

private:
  std::shared_ptr<SmallVector<std::optional<ObjectState>, 8>> ObjectStates;
  std::shared_ptr<RefCount> ArrayRefCount;

  bool Dead = false;

  SmallDenseMap<InvokeInst *, BasicBlock *, 2> ExceptionEdgesToKill;

public:
  PEABlockState();
  PEABlockState(const PEABlockState &Other);
  PEABlockState &operator=(const PEABlockState &Other);
  PEABlockState(PEABlockState &&) = default;
  PEABlockState &operator=(PEABlockState &&) = default;
  ~PEABlockState() = default;

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

  void killExceptionEdge(InvokeInst *I, BasicBlock *UnwindBB);
  bool isExceptionEdgeKilled(InvokeInst *I) const;
  // Returns the underlying mapping for read-only iteration.
  const SmallDenseMap<InvokeInst *, BasicBlock *, 2> &
  killedExceptionEdges() const {
    return ExceptionEdgesToKill;
  }

  bool equivalentTo(const PEABlockState &O) const;

  PEABlockState deepClone() const;

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

  PEAResult() = default;
  PEAResult(const PEAResult &) = delete;
  PEAResult &operator=(const PEAResult &) = delete;
  PEAResult(PEAResult &&) = default;
  PEAResult &operator=(PEAResult &&) = default;
  ~PEAResult();

  ObjectID createVirtualObject(std::unique_ptr<VirtualObject> VO);

  uint32_t nextSeqNo() { return NextSeqNo++; }

  void addBlockEffect(Effect E);

  void clearBlockEffects(ArrayRef<BasicBlock *> Blocks);

  bool hasOptimizationOpportunity() const;
};

} // namespace jeandle
} // namespace llvm

#endif // LLVM_IR_JEANDLE_PARTIALESCAPE_H
