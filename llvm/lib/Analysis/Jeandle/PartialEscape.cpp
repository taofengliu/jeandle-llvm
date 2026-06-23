//===- PartialEscape.cpp - PEA shared data structure impls ----------------===//
//
// Copyright (c) 2026, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Implementation of non-trivial method bodies for the PEA data structures
// declared in llvm/include/llvm/Analysis/Jeandle/PartialEscape.h.

#include "llvm/Analysis/Jeandle/PartialEscape.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Jeandle/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Analysis/Jeandle/PartialEscapeUtils.h"

#include <algorithm>

using namespace llvm;
using namespace llvm::jeandle;

// Per-effect dbgs() trace. When enabled, every Effect routed through
// PEAResult::addBlockEffect emits a one-line summary on dbgs().
// Off by default; turn on with -jeandle-trace-pea. Defined here (rather
// than in PartialEscapeAnalysis.cpp) so the central emission site can
// read it without a separate accessor.
static llvm::cl::opt<bool> JeandleTracePEA(
    "jeandle-trace-pea", llvm::cl::init(false), llvm::cl::Hidden,
    llvm::cl::desc("PEA: emit a one-line dbgs() trace on every major "
                   "effect emission (EliminateAllocation / Materialize / "
                   "ReplaceCall / ReplaceLoad / EliminateStore / "
                   "CreatePHI / ReplaceInput)."));

// ===========================================================================
// VirtualObject
// ===========================================================================

int VirtualObject::getFieldIndex(int64_t Offset, uint8_t ByteSize) const {
  // Fields are kept sorted by Offset; do a binary search for an exact
  // (Offset, ByteSize) match.
  auto It = std::lower_bound(
      Fields.begin(), Fields.end(), Offset,
      [](const FieldDesc &F, int64_t Off) { return F.Offset < Off; });
  if (It == Fields.end() || It->Offset != Offset || It->ByteSize != ByteSize)
    return -1;
  return static_cast<int>(It - Fields.begin());
}

int VirtualObject::getOrCreateFieldIndex(int64_t Offset, Type *Ty) {
  assert(Ty && "field type must be non-null");
  uint8_t ByteSize = 0;
  bool IsReference = false;
  if (Ty->isPointerTy()) {
    // Reference field: ptr addrspace(1). We treat reference fields as pointer
    // sized; the actual byte size depends on target oop size, so we use 8.
    assert(Ty->getPointerAddressSpace() ==
               jeandle::AddrSpace::JavaHeapAddrSpace &&
           "reference field must be in JavaHeapAddrSpace");
    ByteSize = 8;
    IsReference = true;
  } else {
    unsigned Bits = Ty->getPrimitiveSizeInBits();
    assert(Bits > 0 && "field must have a known primitive size");
    ByteSize = static_cast<uint8_t>((Bits + 7) / 8);
  }

  auto It = std::lower_bound(
      Fields.begin(), Fields.end(), Offset,
      [](const FieldDesc &F, int64_t Off) { return F.Offset < Off; });

  // Exact match? Reuse if the existing FieldDesc agrees on size and
  // reference-ness.
  if (It != Fields.end() && It->Offset == Offset) {
    if (It->ByteSize != ByteSize || It->IsReference != IsReference)
      return -1;
    return static_cast<int>(It - Fields.begin());
  }

  // Overlap check against the candidate neighbor on the left.
  if (It != Fields.begin()) {
    auto Prev = std::prev(It);
    if (Prev->overlaps(Offset, ByteSize))
      return -1;
  }
  // Overlap check against the candidate neighbor on the right.
  if (It != Fields.end() && It->overlaps(Offset, ByteSize))
    return -1;

  FieldDesc New{Offset, Ty, ByteSize, IsReference};
  auto NewIt = Fields.insert(It, New);
  return static_cast<int>(NewIt - Fields.begin());
}

int VirtualObject::getArrayLengthFieldIndex() const {
  assert(Kind == Array);
  return getFieldIndex(ArrayLengthSlotOffset, /*ByteSize=*/4);
}

int64_t VirtualObject::arrayElementOffset(uint32_t Index) const {
  assert(Kind == Array);
  if (Index >= ArrayLength)
    return -1;
  return static_cast<int64_t>(ArrayBaseOffset) +
         static_cast<int64_t>(Index) * static_cast<int64_t>(ArrayIndexScale);
}

// Strip identity-preserving wrappers (freeze, bitcast, zext, sext) from an
// index Value. Used by matchArrayElementGEP to canonicalize the index so
// callers can pattern-match a ConstantInt or a Value identity.
static Value *peelIndexWrappers(Value *V) {
  for (unsigned Depth = 0; Depth < 8; ++Depth) {
    if (!V)
      return V;
    if (auto *FI = dyn_cast<FreezeInst>(V)) {
      V = FI->getOperand(0);
      continue;
    }
    if (auto *BC = dyn_cast<BitCastOperator>(V)) {
      // BitCast on an integer (the index slot) is identity-preserving.
      V = BC->getOperand(0);
      continue;
    }
    if (auto *ZI = dyn_cast<ZExtInst>(V)) {
      V = ZI->getOperand(0);
      continue;
    }
    if (auto *SI = dyn_cast<SExtInst>(V)) {
      V = SI->getOperand(0);
      continue;
    }
    // Handle the ConstantExpr forms in case they appear here.
    if (auto *CE = dyn_cast<ConstantExpr>(V)) {
      if (CE->getOpcode() == Instruction::ZExt ||
          CE->getOpcode() == Instruction::SExt ||
          CE->getOpcode() == Instruction::BitCast) {
        V = CE->getOperand(0);
        continue;
      }
    }
    break;
  }
  return V;
}

// Match the symbolic byte-offset pattern emitted by the abstract
// interpreter for indexed accesses on an i8-typed GEP: byteOff equals
// `ArrayBaseOffset + idx * scale`, where the scaling is expressed as a
// shift by log2(scale) or a multiply by scale. Returns the (peeled) index
// Value on success, or nullptr on no match.
static Value *matchAddBasePlusScaledIndex(Value *ByteOff,
                                          int64_t ExpectedBaseOffset,
                                          uint64_t Scale) {
  using namespace llvm::PatternMatch;
  if (!ByteOff || !ByteOff->getType()->isIntegerTy())
    return nullptr;
  if (Scale == 0)
    return nullptr;

  // Recognise the constant byte-offset operand of an `add C, X` pair.
  Value *ScaledIdx = nullptr;
  ConstantInt *BaseCI = nullptr;
  if (match(ByteOff, m_Add(m_ConstantInt(BaseCI), m_Value(ScaledIdx)))) {
    // matched
  } else if (match(ByteOff, m_Add(m_Value(ScaledIdx), m_ConstantInt(BaseCI)))) {
    // matched
  } else {
    return nullptr;
  }
  if (!BaseCI || BaseCI->getSExtValue() != ExpectedBaseOffset)
    return nullptr;

  // Now ScaledIdx must equal idx * Scale (or idx if Scale == 1).
  if (Scale == 1)
    return peelIndexWrappers(ScaledIdx);

  Value *Idx = nullptr;
  if (isPowerOf2_64(Scale)) {
    unsigned LogScale = Log2_64(Scale);
    ConstantInt *ShAmt = nullptr;
    if (match(ScaledIdx, m_Shl(m_Value(Idx), m_ConstantInt(ShAmt))) &&
        ShAmt && ShAmt->getZExtValue() == LogScale)
      return peelIndexWrappers(Idx);
  }
  ConstantInt *MulCI = nullptr;
  if (match(ScaledIdx, m_Mul(m_Value(Idx), m_ConstantInt(MulCI))) &&
      MulCI && (uint64_t)MulCI->getSExtValue() == Scale)
    return peelIndexWrappers(Idx);
  if (match(ScaledIdx, m_Mul(m_ConstantInt(MulCI), m_Value(Idx))) &&
      MulCI && (uint64_t)MulCI->getSExtValue() == Scale)
    return peelIndexWrappers(Idx);
  return nullptr;
}

std::optional<VirtualObject::ArrayElementGEPMatch>
VirtualObject::matchArrayElementGEP(GetElementPtrInst *GEP,
                                    const DataLayout &DL) const {
  if (!GEP)
    return std::nullopt;
  if (Kind != Array || !ArrayElementType || ArrayIndexScale == 0)
    return std::nullopt;

  auto *GEPOp = cast<GEPOperator>(GEP);
  const unsigned AS = GEPOp->getPointerAddressSpace();
  const unsigned PtrBits = DL.getPointerSizeInBits(AS);

  // Pattern A: typed GEP with sourceElementType == ArrayElementType. The
  // typed GEP's pointer operand must reach the alloc base with a constant
  // accumulated byte offset equal to ArrayBaseOffset (Jeandle emits this
  // as a preceding i8 + base-offset GEP).
  if (GEPOp->getSourceElementType() == ArrayElementType &&
      GEPOp->getNumIndices() == 1) {
    int64_t BaseOff = 0;
    bool NonConst = false;
    (void)jeandle::pea::stripPointerCastsAndOffsets(
        GEPOp->getPointerOperand(), DL, &BaseOff, &NonConst);
    if (!NonConst && BaseOff == static_cast<int64_t>(ArrayBaseOffset))
      return ArrayElementGEPMatch{peelIndexWrappers(GEPOp->getOperand(1)),
                                  ArrayElementType};
  }

  // Pattern B: i8 GEP with a single byte-offset index. The byte offset may
  // be (a) a constant equal to ArrayBaseOffset + cidx * Scale, or (b) an
  // `add ArrayBaseOffset, (shl|mul) idx, Scale`. Pattern (a) is handled
  // naturally by resolveFieldOffset (constant byte offset), so the
  // matcher's main job here is pattern (b) for symbolic indices, but we
  // still recognise (a) and surface a ConstantInt-wrapped element index.
  if (GEPOp->getSourceElementType()->isIntegerTy(8) &&
      GEPOp->getNumIndices() == 1) {
    int64_t BaseOff = 0;
    bool NonConst = false;
    (void)jeandle::pea::stripPointerCastsAndOffsets(
        GEPOp->getPointerOperand(), DL, &BaseOff, &NonConst);
    if (NonConst || BaseOff != 0)
      return std::nullopt;

    Value *ByteOff = GEPOp->getOperand(1);
    if (auto *CI = dyn_cast<ConstantInt>(ByteOff)) {
      // Constant byte offset. Recover the element index if (off - base) is
      // a multiple of Scale and lies in [0, ArrayLength).
      int64_t Off = CI->getValue().sextOrTrunc(PtrBits).getSExtValue();
      int64_t Adj = Off - static_cast<int64_t>(ArrayBaseOffset);
      if (Adj < 0 || (Adj % static_cast<int64_t>(ArrayIndexScale)) != 0)
        return std::nullopt;
      int64_t Cidx = Adj / static_cast<int64_t>(ArrayIndexScale);
      if (Cidx < 0 || static_cast<uint64_t>(Cidx) >= ArrayLength)
        return std::nullopt;
      Constant *CIdx =
          ConstantInt::get(CI->getType(), Cidx, /*isSigned=*/true);
      return ArrayElementGEPMatch{CIdx, ArrayElementType};
    }

    if (Value *Idx = matchAddBasePlusScaledIndex(
            ByteOff, static_cast<int64_t>(ArrayBaseOffset), ArrayIndexScale))
      return ArrayElementGEPMatch{Idx, ArrayElementType};
  }

  return std::nullopt;
}

Type *VirtualObject::getMaterializedType(LLVMContext &Ctx) {
  return PointerType::get(Ctx, jeandle::AddrSpace::JavaHeapAddrSpace);
}

std::unique_ptr<VirtualObject> VirtualObject::duplicate() const {
  // The clone is detached: ID is set to InvalidObjectID and the caller is
  // expected to register it via PEAResult::createVirtualObject to obtain a
  // fresh ID.
  auto Clone =
      std::make_unique<VirtualObject>(InvalidObjectID, Kind, AllocationCall);
  Clone->Klass = Klass;
  Clone->SizeInBytes = SizeInBytes;
  Clone->ArrayLength = ArrayLength;
  Clone->ArrayElementType = ArrayElementType;
  Clone->ArrayIndexScale = ArrayIndexScale;
  Clone->ArrayBaseOffset = ArrayBaseOffset;
  Clone->Fields = Fields;
  Clone->IsSingleUsageAllocation = false;
  Clone->IdentityHashObserved = IdentityHashObserved;
  // Carry the boxed-primitive tag across duplicate() so the synthetic
  // Case-C VO produced by synthesizeCaseC inherits the boxed kind from its
  // per-pred sources (we only enter the boxed-merge identity-bail-drop
  // path when every per-pred VO carries the same BoxedPrimitiveKind).
  Clone->BoxedPrimitiveKind = BoxedPrimitiveKind;
  // Synthetic-state fields are NOT copied — duplicate() is shared by the
  // generic VirtualObject clone path AND the Case C synthesis path; the
  // latter sets IsSynthetic/SyntheticSourceIDs/SyntheticPhi explicitly after
  // calling duplicate.
  Clone->IsSynthetic = false;
  Clone->SyntheticPhi = nullptr;
  return Clone;
}

// ===========================================================================
// FieldValue
// ===========================================================================

FieldValue FieldValue::scalar(Value *V) {
  assert(V && "scalar value must be non-null");
  FieldValue F;
  F.T = Scalar;
  F.U.V = V;
  F.DeclaredType = V->getType();
  return F;
}

FieldValue FieldValue::virtualRef(ObjectID ID, Type *RefTy) {
  assert(ID != InvalidObjectID);
  assert(RefTy && RefTy->isPointerTy() &&
         RefTy->getPointerAddressSpace() == jeandle::AddrSpace::JavaHeapAddrSpace &&
         "virtualRef DeclaredType must be ptr addrspace(1)");
  FieldValue F;
  F.T = VirtualRef;
  F.U.Ref = ID;
  F.DeclaredType = RefTy;
  return F;
}

FieldValue FieldValue::materializedRef(Value *Ptr) {
  assert(Ptr && Ptr->getType()->isPointerTy());
  FieldValue F;
  F.T = MaterializedRef;
  F.U.V = Ptr;
  F.DeclaredType = Ptr->getType();
  return F;
}

Constant *FieldValue::defaultFor(Type *FieldType) {
  assert(FieldType);
  if (FieldType->isPointerTy()) {
    assert(FieldType->getPointerAddressSpace() ==
               jeandle::AddrSpace::JavaHeapAddrSpace &&
           "reference default must be in JavaHeapAddrSpace");
    return ConstantPointerNull::get(cast<PointerType>(FieldType));
  }
  return Constant::getNullValue(FieldType);
}

bool FieldValue::shallowEquals(const FieldValue &O) const {
  if (T != O.T)
    return false;
  switch (T) {
  case Unknown:
    return true;
  case Scalar:
  case MaterializedRef:
    return U.V == O.U.V;
  case VirtualRef:
    return U.Ref == O.U.Ref;
  }
  return false;
}

llvm::hash_code FieldValue::hash() const {
  switch (T) {
  case Unknown:
    return llvm::hash_combine(static_cast<uint8_t>(T));
  case Scalar:
  case MaterializedRef:
    return llvm::hash_combine(static_cast<uint8_t>(T), U.V);
  case VirtualRef:
    return llvm::hash_combine(static_cast<uint8_t>(T), U.Ref);
  }
  return llvm::hash_value(0);
}

// ===========================================================================
// ObjectState
// ===========================================================================

ObjectState ObjectState::clone() const {
  ObjectState Copy(0);
  Copy.Kind = Kind;
  Copy.Entries = Entries;
  Copy.Locks = Locks;
  Copy.MaterializedValue = MaterializedValue;
  Copy.CopyOnWrite = false;
  return Copy;
}

llvm::hash_code ObjectState::hash() const {
  llvm::hash_code H = llvm::hash_combine(static_cast<uint8_t>(Kind),
                                          static_cast<unsigned>(Locks.size()));
  // Fold each (EnterCall, BytecodeDepth) into the hash so two distinct lock
  // stacks with the same size do not collide. Matches equivalentTo's compare.
  for (const auto &L : Locks)
    H = llvm::hash_combine(H, L.EnterCall, L.BytecodeDepth);
  if (Kind == Materialized) {
    return llvm::hash_combine(H, MaterializedValue);
  }
  for (const auto &E : Entries)
    H = llvm::hash_combine(H, E.hash());
  return H;
}

// ===========================================================================
// PEABlockState
// ===========================================================================

// Mark every present ObjectState in the given vector as "shared". Called at
// the moment our backing vector goes from sole-ownership (Count==1) to
// shared (Count==2).
static void
markAllSlotsShared(const SmallVector<std::optional<ObjectState>, 8> &Arr) {
  for (const auto &Slot : Arr) {
    if (Slot)
      Slot->markShared();
  }
}

PEABlockState::PEABlockState()
    : ObjectStates(std::make_shared<
                   SmallVector<std::optional<ObjectState>, 8>>()),
      ArrayRefCount(std::make_shared<RefCount>()) {}

PEABlockState::PEABlockState(const PEABlockState &Other)
    : ObjectStates(Other.ObjectStates), ArrayRefCount(Other.ArrayRefCount),
      Dead(Other.Dead) {
  if (ArrayRefCount) {
    // "Share handshake": if the array is about to transition from
    // sole-ownership to shared (Count 1 -> 2), mark every slot as
    // CopyOnWrite so a later per-slot mutation triggers a per-slot clone
    // rather than stomping the value that the now-shared peer can still see.
    if (ArrayRefCount->Count == 1 && ObjectStates)
      markAllSlotsShared(*ObjectStates);
    ++ArrayRefCount->Count;
  }
}

PEABlockState::~PEABlockState() {
  // Decrement the logical Count so survivors (other PEABlockStates that
  // share this array) see an accurate sharer total. A moved-from instance
  // has nullified shared_ptrs and skips this path.
  if (ArrayRefCount) {
    assert(ArrayRefCount->Count >= 1 && "refcount underflow at destruction");
    --ArrayRefCount->Count;
  }
}

PEABlockState &PEABlockState::operator=(PEABlockState &&Other) noexcept {
  if (this == &Other)
    return *this;
  // Drop *this's old logical ref before overwriting. If we already shared
  // with Other (same RefCount), the decrement-then-move-in still produces
  // the right final Count (sole holder == 1) because Other's shared_ptr is
  // moved out and won't double-decrement on its destruction.
  if (ArrayRefCount) {
    assert(ArrayRefCount->Count >= 1 && "refcount underflow on move-assign");
    --ArrayRefCount->Count;
  }
  ObjectStates = std::move(Other.ObjectStates);
  ArrayRefCount = std::move(Other.ArrayRefCount);
  Dead = Other.Dead;
  return *this;
}

PEABlockState &PEABlockState::operator=(const PEABlockState &Other) {
  if (this == &Other)
    return *this;
  // ObjectStates and ArrayRefCount move in lockstep: if Other's RefCount is
  // already ours, the count is already accurate and we skip the
  // decrement/increment dance.
  if (ArrayRefCount != Other.ArrayRefCount) {
    if (ArrayRefCount) {
      assert(ArrayRefCount->Count >= 1 && "refcount underflow on drop");
      --ArrayRefCount->Count;
    }
    ObjectStates = Other.ObjectStates;
    ArrayRefCount = Other.ArrayRefCount;
    if (ArrayRefCount) {
      if (ArrayRefCount->Count == 1 && ObjectStates)
        markAllSlotsShared(*ObjectStates);
      ++ArrayRefCount->Count;
    }
  }
  Dead = Other.Dead;
  return *this;
}

SmallVector<std::optional<ObjectState>, 8> *
PEABlockState::getArrayForModification() {
  // Outer / array-level COW (level 1 of the "two-level" scheme). Inner /
  // per-slot COW (level 2) lives in getObjectStateForModification, where
  // a slot still flagged isShared() after the array clone is duplicated
  // before the caller writes through the returned reference.
  //
  // Because Jeandle stores `SmallVector<optional<ObjectState>>` by value,
  // the vector copy at the array level already deep-copies each slot. The
  // per-slot CopyOnWrite flag therefore mostly serves as a one-shot tag —
  // copies inherit `CopyOnWrite=true`, the next mutator clones once and
  // resets it to false (ObjectState's custom copy ctor), and subsequent
  // in-place writes are free. The savings vs. always-clone come entirely
  // from level 1 in the common case where a block state is snapshotted /
  // adopted and then either never mutated (loop-fixpoint convergence) or
  // mutated only sparsely.
  if (!ObjectStates) {
    // Defensive: a default-constructed PEABlockState always allocates an
    // empty vector, but operator=/move could in principle leave us null.
    assert(!ArrayRefCount &&
           "ObjectStates and ArrayRefCount must be in lockstep");
    ObjectStates =
        std::make_shared<SmallVector<std::optional<ObjectState>, 8>>();
    ArrayRefCount = std::make_shared<RefCount>();
    return ObjectStates.get();
  }
  assert(ArrayRefCount && "ObjectStates and ArrayRefCount must be in lockstep");
  if (ArrayRefCount->Count > 1) {
    --ArrayRefCount->Count;
    ObjectStates = std::make_shared<SmallVector<std::optional<ObjectState>, 8>>(
        *ObjectStates);
    ArrayRefCount = std::make_shared<RefCount>();
  }
  // After detach we must be the sole logical owner. ObjectStates may still
  // have shared_ptr::use_count() > 1 if a snapshot copy exists, but our
  // RefCount must be 1 since nobody else points to it.
  assert(ArrayRefCount->Count == 1 &&
         "getArrayForModification did not produce sole ownership");
  return ObjectStates.get();
}

void PEABlockState::ensureSize(unsigned Size) {
  auto *Arr = getArrayForModification();
  if (Arr->size() < Size)
    Arr->resize(Size);
}

void PEABlockState::addObject(ObjectID ID, ObjectState State) {
  assert(ID != InvalidObjectID);
  // Route the write through the helper-returned pointer so we never touch
  // the shared_ptr's raw payload before COW has had a chance to detach us.
  auto *Arr = getArrayForModification();
  if (Arr->size() < ID + 1)
    Arr->resize(ID + 1);
  (*Arr)[ID] = std::move(State);
}

bool PEABlockState::hasObjectState(ObjectID ID) const {
  if (!ObjectStates || ID >= ObjectStates->size())
    return false;
  return (*ObjectStates)[ID].has_value();
}

const ObjectState &PEABlockState::getObjectState(ObjectID ID) const {
  assert(hasObjectState(ID));
  return *(*ObjectStates)[ID];
}

const ObjectState *PEABlockState::getObjectStateOptional(ObjectID ID) const {
  if (!ObjectStates || ID >= ObjectStates->size())
    return nullptr;
  const auto &Slot = (*ObjectStates)[ID];
  return Slot ? &*Slot : nullptr;
}

ObjectState &PEABlockState::getObjectStateForModification(ObjectID ID) {
  assert(ID != InvalidObjectID);
  auto *Arr = getArrayForModification();
  assert(ID < Arr->size() && (*Arr)[ID].has_value() &&
         "object not registered in this block state");
  auto &Slot = (*Arr)[ID];
  // Level-2 COW. After the array detach above we may still own a slot whose
  // CopyOnWrite tag was carried over from the time the vector was shared.
  // Clone the inner ObjectState before handing out a mutable reference;
  // ObjectState's custom copy ctor resets CopyOnWrite=false so subsequent
  // mutations of THIS slot are free.
  if (Slot->isShared()) {
    ObjectState Cloned = Slot->clone();
    *Slot = std::move(Cloned);
    assert(!Slot->isShared() && "clone must reset CopyOnWrite");
  }
  return *Slot;
}

void PEABlockState::resetObjectStates(unsigned NumObjects) {
  // Drop our hold on whatever backing vector we currently share; allocate a
  // brand-new array + RefCount{1} that nobody else points at.
  if (ArrayRefCount) {
    assert(ArrayRefCount->Count >= 1 && "refcount underflow on reset");
    --ArrayRefCount->Count;
  }
  ObjectStates =
      std::make_shared<SmallVector<std::optional<ObjectState>, 8>>(NumObjects);
  ArrayRefCount = std::make_shared<RefCount>();
}

void PEABlockState::adoptObjectStates(const PEABlockState &Other) {
  // True shared_ptr handoff. No deep clone — we point at Other's backing
  // vector AND Other's RefCount, then bump the count. On first share
  // (Count was 1, becomes 2) mark every present ObjectState as
  // CopyOnWrite so subsequent per-slot mutations on either side go through
  // a slot clone rather than corrupting the peer's view.
  if (!Other.ObjectStates) {
    resetObjectStates(0);
    return;
  }
  // Short-circuit if we already share with Other; the count is already
  // accurate and there is no work to do.
  if (ArrayRefCount == Other.ArrayRefCount)
    return;
  // Drop the ref we currently hold (typically the empty vector + Count{1}
  // allocated by the default ctor that runs immediately before
  // adoptObjectStates in the analyzer's per-block setup).
  if (ArrayRefCount) {
    assert(ArrayRefCount->Count >= 1 && "refcount underflow on adopt");
    --ArrayRefCount->Count;
  }
  ObjectStates = Other.ObjectStates;
  ArrayRefCount = Other.ArrayRefCount;
  assert(ArrayRefCount && "Other had a backing vector but no RefCount?");
  if (ArrayRefCount->Count == 1)
    markAllSlotsShared(*ObjectStates);
  ++ArrayRefCount->Count;
}

std::optional<ObjectID>
PEABlockState::resolveVirtualRef(Value *V, const AliasMap &Aliases) const {
  // We need a DataLayout for the GEP-walking inside pea::resolveVirtualRef.
  // Find it via V's parent Module if possible; otherwise we cannot resolve.
  if (!V)
    return std::nullopt;
  Module *M = nullptr;
  if (auto *I = dyn_cast<Instruction>(V)) {
    if (BasicBlock *BB = I->getParent())
      if (Function *F = BB->getParent())
        M = F->getParent();
  } else if (auto *A = dyn_cast<Argument>(V)) {
    if (Function *F = A->getParent())
      M = F->getParent();
  }
  if (!M)
    return std::nullopt;
  return pea::resolveVirtualRef(V, *this, Aliases, M->getDataLayout());
}

// ===========================================================================
// AliasMap
// ===========================================================================

void AliasMap::addVirtualAlias(Value *V, ObjectID ID) {
  assert(V && ID != InvalidObjectID);
  assert(!VirtualAliases.count(V) && "value already aliased");
  VirtualAliases[V] = ID;
  for (User *U : V->users()) {
    if (auto *I = dyn_cast<Instruction>(U))
      HasVirtualInputs.insert(I);
  }
}

void AliasMap::addScalarAlias(Value *V, Value *Replacement) {
  assert(V && Replacement);
  ScalarAliases[V] = Replacement;
  for (User *U : V->users()) {
    if (auto *I = dyn_cast<Instruction>(U))
      HasScalarReplacedInputs.insert(I);
  }
}

void AliasMap::resetAlias(Value *V) {
  VirtualAliases.erase(V);
  ScalarAliases.erase(V);
}

std::optional<ObjectID> AliasMap::getVirtualAlias(Value *V) const {
  auto It = VirtualAliases.find(V);
  if (It == VirtualAliases.end())
    return std::nullopt;
  return It->second;
}

Value *AliasMap::getScalarAlias(Value *V) const {
  auto It = ScalarAliases.find(V);
  return It == ScalarAliases.end() ? nullptr : It->second;
}

Value *AliasMap::resolve(Value *V, const PEABlockState &State) const {
  if (!V)
    return nullptr;
  // Scalar replacement chain.
  if (Value *S = getScalarAlias(V))
    return S;
  // Virtual alias: if the object is already materialized on this path, return
  // the materialized pointer; otherwise return V unchanged (callers that
  // care about the virtual identity must consult getVirtualAlias directly).
  if (auto ID = getVirtualAlias(V)) {
    if (const ObjectState *OS = State.getObjectStateOptional(*ID)) {
      if (OS->isMaterialized())
        return OS->getMaterializedValue();
    }
  }
  return V;
}

void AliasMap::clear() {
  VirtualAliases.clear();
  ScalarAliases.clear();
  HasVirtualInputs.clear();
  HasScalarReplacedInputs.clear();
}

AliasMap AliasMap::snapshot() const { return *this; }

void AliasMap::restore(const AliasMap &S) { *this = S; }

void AliasMap::invalidate(Value *V) {
  VirtualAliases.erase(V);
  ScalarAliases.erase(V);
  if (auto *I = dyn_cast_or_null<Instruction>(V)) {
    HasVirtualInputs.erase(I);
    HasScalarReplacedInputs.erase(I);
  }
}

// ===========================================================================
// PEAResult
// ===========================================================================

PEAResult::~PEAResult() {
  // Any unparented PHI created by the analyzer (e.g., the analyzer ran
  // but the transform never consumed the result) must be freed. Once a PHI
  // has been inserted into a BasicBlock, that block's ilist owns it and we
  // must NOT delete here. WeakTrackingVH auto-nulls when the underlying
  // Value is deleted by an unrelated path (e.g. dead-code sweep), so the
  // null-check below also guards against stale references.
  for (WeakTrackingVH &VH : OwnedPhis) {
    if (Value *V = VH) {
      if (auto *Phi = dyn_cast<PHINode>(V))
        if (!Phi->getParent())
          delete Phi;
    }
  }
  // Same lifecycle for ReplaceLoad coercion instructions.
  for (WeakTrackingVH &VH : OwnedInsts) {
    if (Value *V = VH) {
      if (auto *I = dyn_cast<Instruction>(V))
        if (!I->getParent())
          I->deleteValue();
    }
  }
  // Loop-header field PHIs preserved across fixpoint iterations.
  for (WeakTrackingVH &VH : OwnedLoopFieldPhis) {
    if (Value *V = VH) {
      if (auto *Phi = dyn_cast<PHINode>(V))
        if (!Phi->getParent())
          delete Phi;
    }
  }
}

ObjectID PEAResult::createVirtualObject(std::unique_ptr<VirtualObject> VO) {
  assert(VO);
  ObjectID ID = static_cast<ObjectID>(VirtualObjects.size());
  // Re-stamp the ID. VirtualObject::ID is const, so use placement-new to
  // construct a fresh instance with the correct ID while keeping the rest of
  // the state intact.
  auto Stamped =
      std::make_unique<VirtualObject>(ID, VO->getKind(), VO->AllocationCall);
  Stamped->Klass = VO->Klass;
  Stamped->SizeInBytes = VO->SizeInBytes;
  Stamped->ArrayLength = VO->ArrayLength;
  Stamped->ArrayElementType = VO->ArrayElementType;
  Stamped->ArrayIndexScale = VO->ArrayIndexScale;
  Stamped->ArrayBaseOffset = VO->ArrayBaseOffset;
  Stamped->Fields = std::move(VO->Fields);
  Stamped->IsSingleUsageAllocation = VO->IsSingleUsageAllocation;
  Stamped->IdentityHashObserved = VO->IdentityHashObserved;
  // Propagate the boxed-primitive tag installed by tier1Allocate (and
  // inherited by the synthetic Case-C clone via duplicate()) into the
  // re-stamped VO so downstream queries see the correct kind.
  Stamped->BoxedPrimitiveKind = VO->BoxedPrimitiveKind;
  VirtualObjects.push_back(std::move(Stamped));
  return ID;
}

void PEAResult::addBlockEffect(Effect E) {
  assert(E.Block);
  // Per-effect trace (gated on -jeandle-trace-pea, off by default).
  // Centralised here so every emission site routes through a single trace
  // call. The trace identifies the effect kind, the owning ObjectID (when
  // set), the block
  // label, and a short Target/Replacement summary so a `2>&1 | grep PEA:`
  // sweep is enough to follow the analyser's decisions.
  if (JeandleTracePEA) {
    auto effectKindName = [](EffectKind K) -> const char * {
      switch (K) {
      case EffectKind::ReplaceLoad: return "ReplaceLoad";
      case EffectKind::ReplaceCall: return "ReplaceCall";
      case EffectKind::ReplaceInput: return "ReplaceInput";
      case EffectKind::EliminateStore: return "EliminateStore";
      case EffectKind::EliminateAllocation: return "EliminateAllocation";
      case EffectKind::Materialize: return "Materialize";
      case EffectKind::CreatePHI: return "CreatePHI";
      }
      return "Unknown";
    };
    llvm::dbgs() << "PEA: " << effectKindName(E.Kind);
    if (E.ObjID != InvalidObjectID)
      llvm::dbgs() << " [VO=" << static_cast<unsigned>(E.ObjID) << "]";
    if (E.Block && E.Block->hasName())
      llvm::dbgs() << " block=%" << E.Block->getName();
    if (E.Target)
      llvm::dbgs() << " target=" << *E.Target;
    llvm::dbgs() << "\n";
  }
  BlockEffects[E.Block].push_back(std::move(E));
}

bool PEAResult::hasOptimizationOpportunity() const {
  return VirtualizationDelta > 0 || AllocationDelta != 0 ||
         !BlockEffects.empty();
}
