//===-- PartialEscape.cpp - PEA shared data structure impls --------------===//
//
// Part of the Jeandle JIT compiler.
//
// Implementation of non-trivial method bodies for the PEA data structures
// declared in llvm/include/llvm/IR/Jeandle/PartialEscape.h.
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/Jeandle/PartialEscape.h"

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
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/Transforms/Jeandle/PartialEscapeUtils.h"

#include <algorithm>

using namespace llvm;
using namespace llvm::jeandle;

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
  // reference-ness (per Section 1.1 type coercion policy).
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

std::optional<uint32_t>
VirtualObject::matchArrayElementGEP(GetElementPtrInst *GEP) const {
  // Shape-only stub. The canonical pattern detector requires
  // abstract-interpreter context to validate it.
  (void)GEP;
  return std::nullopt;
}

unsigned VirtualObject::entryCount() const {
  if (Kind == Instance)
    return static_cast<unsigned>(Fields.size());
  return ArrayLength;
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
  Clone->ArrayLengthVal = ArrayLengthVal;
  Clone->ArrayLength = ArrayLength;
  Clone->ArrayElementType = ArrayElementType;
  Clone->ArrayIndexScale = ArrayIndexScale;
  Clone->ArrayBaseOffset = ArrayBaseOffset;
  Clone->Fields = Fields;
  Clone->IsSingleUsageAllocation = false;
  Clone->IdentityHashObserved = IdentityHashObserved;
  Clone->MustPreserveLocks = MustPreserveLocks;
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
  Copy.LockCount = LockCount;
  Copy.MaterializedValue = MaterializedValue;
  Copy.CopyOnWrite = false;
  return Copy;
}

bool ObjectState::equivalentTo(const ObjectState &O) const {
  if (Kind != O.Kind)
    return false;
  if (LockCount != O.LockCount)
    return false;
  if (Kind == Materialized)
    return MaterializedValue == O.MaterializedValue;
  // Virtual
  if (Entries.size() != O.Entries.size())
    return false;
  for (size_t I = 0, E = Entries.size(); I < E; ++I) {
    if (!Entries[I].shallowEquals(O.Entries[I]))
      return false;
  }
  return true;
}

llvm::hash_code ObjectState::hash() const {
  llvm::hash_code H = llvm::hash_combine(static_cast<uint8_t>(Kind), LockCount);
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

PEABlockState::PEABlockState()
    : ObjectStates(std::make_shared<
                   SmallVector<std::optional<ObjectState>, 8>>()),
      ArrayRefCount(std::make_shared<RefCount>()) {}

PEABlockState::PEABlockState(const PEABlockState &Other)
    : ObjectStates(Other.ObjectStates), ArrayRefCount(Other.ArrayRefCount),
      Dead(Other.Dead), ExceptionEdgesToKill(Other.ExceptionEdgesToKill) {
  if (ArrayRefCount)
    ++ArrayRefCount->Count;
}

PEABlockState &PEABlockState::operator=(const PEABlockState &Other) {
  if (this == &Other)
    return *this;
  ObjectStates = Other.ObjectStates;
  if (ArrayRefCount && ArrayRefCount != Other.ArrayRefCount) {
    // Drop the old refcount: the shared_ptr destructor handles lifetime; we
    // only need to decrement our logical Count.
    --ArrayRefCount->Count;
  }
  ArrayRefCount = Other.ArrayRefCount;
  if (ArrayRefCount)
    ++ArrayRefCount->Count;
  Dead = Other.Dead;
  ExceptionEdgesToKill = Other.ExceptionEdgesToKill;
  return *this;
}

SmallVector<std::optional<ObjectState>, 8> *
PEABlockState::getArrayForModification() {
  // TODO: two-level COW optimization. For now, always clone when the
  // underlying vector is shared with anyone else. The shape (shared_ptr +
  // RefCount) is in place so a proper COW can be layered on top without
  // touching callers.
  if (!ObjectStates) {
    ObjectStates =
        std::make_shared<SmallVector<std::optional<ObjectState>, 8>>();
    ArrayRefCount = std::make_shared<RefCount>();
    return ObjectStates.get();
  }
  if (ArrayRefCount && ArrayRefCount->Count > 1) {
    --ArrayRefCount->Count;
    ObjectStates = std::make_shared<SmallVector<std::optional<ObjectState>, 8>>(
        *ObjectStates);
    ArrayRefCount = std::make_shared<RefCount>();
  }
  return ObjectStates.get();
}

void PEABlockState::ensureSize(unsigned Size) {
  auto *Arr = getArrayForModification();
  if (Arr->size() < Size)
    Arr->resize(Size);
}

void PEABlockState::addObject(ObjectID ID, ObjectState State) {
  assert(ID != InvalidObjectID);
  ensureSize(ID + 1);
  (*ObjectStates)[ID] = std::move(State);
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
  // Element-level COW: clone if shared.
  if (Slot->isShared()) {
    ObjectState Cloned = Slot->clone();
    *Slot = std::move(Cloned);
  }
  return *Slot;
}

void PEABlockState::resetObjectStates(unsigned NumObjects) {
  if (ArrayRefCount && ArrayRefCount->Count > 1) {
    --ArrayRefCount->Count;
  }
  ObjectStates =
      std::make_shared<SmallVector<std::optional<ObjectState>, 8>>(NumObjects);
  ArrayRefCount = std::make_shared<RefCount>();
}

void PEABlockState::adoptObjectStates(const PEABlockState &Other) {
  // TODO: two-level COW optimization. Currently we always deep-clone the
  // adopted vector, but still mark the source's ObjectStates as shared to
  // preserve the invariant that adopted states are CopyOnWrite from the
  // caller's perspective. A future change can swap the deep clone for a
  // real shared_ptr handoff.
  if (!Other.ObjectStates) {
    resetObjectStates(0);
    return;
  }
  for (const auto &Slot : *Other.ObjectStates) {
    if (Slot)
      Slot->markShared();
  }
  if (ArrayRefCount && ArrayRefCount->Count > 1) {
    --ArrayRefCount->Count;
  }
  ObjectStates = std::make_shared<SmallVector<std::optional<ObjectState>, 8>>(
      *Other.ObjectStates);
  ArrayRefCount = std::make_shared<RefCount>();
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

void PEABlockState::killExceptionEdge(InvokeInst *I, BasicBlock *UnwindBB) {
  assert(I && UnwindBB);
  ExceptionEdgesToKill[I] = UnwindBB;
}

bool PEABlockState::isExceptionEdgeKilled(InvokeInst *I) const {
  return ExceptionEdgesToKill.count(I);
}

bool PEABlockState::equivalentTo(const PEABlockState &O) const {
  if (Dead != O.Dead)
    return false;
  unsigned N = std::max(getStateCount(), O.getStateCount());
  for (unsigned I = 0; I < N; ++I) {
    const ObjectState *A = getObjectStateOptional(I);
    const ObjectState *B = O.getObjectStateOptional(I);
    if (!A && !B)
      continue;
    if (!A || !B)
      return false;
    if (!A->equivalentTo(*B))
      return false;
  }
  if (ExceptionEdgesToKill.size() != O.ExceptionEdgesToKill.size())
    return false;
  for (const auto &KV : ExceptionEdgesToKill) {
    auto It = O.ExceptionEdgesToKill.find(KV.first);
    if (It == O.ExceptionEdgesToKill.end() || It->second != KV.second)
      return false;
  }
  return true;
}

PEABlockState PEABlockState::deepClone() const {
  PEABlockState Copy;
  Copy.Dead = Dead;
  Copy.ExceptionEdgesToKill = ExceptionEdgesToKill;
  if (ObjectStates) {
    auto NewArr =
        std::make_shared<SmallVector<std::optional<ObjectState>, 8>>();
    NewArr->reserve(ObjectStates->size());
    for (const auto &Slot : *ObjectStates) {
      if (Slot)
        NewArr->push_back(Slot->clone());
      else
        NewArr->push_back(std::nullopt);
    }
    // Drop the freshly-constructed default vector.
    if (Copy.ArrayRefCount && Copy.ArrayRefCount->Count > 1)
      --Copy.ArrayRefCount->Count;
    Copy.ObjectStates = std::move(NewArr);
    Copy.ArrayRefCount = std::make_shared<RefCount>();
  }
  return Copy;
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
  Stamped->ArrayLengthVal = VO->ArrayLengthVal;
  Stamped->ArrayLength = VO->ArrayLength;
  Stamped->ArrayElementType = VO->ArrayElementType;
  Stamped->ArrayIndexScale = VO->ArrayIndexScale;
  Stamped->ArrayBaseOffset = VO->ArrayBaseOffset;
  Stamped->Fields = std::move(VO->Fields);
  Stamped->IsSingleUsageAllocation = VO->IsSingleUsageAllocation;
  Stamped->IdentityHashObserved = VO->IdentityHashObserved;
  Stamped->MustPreserveLocks = VO->MustPreserveLocks;
  VirtualObjects.push_back(std::move(Stamped));
  return ID;
}

void PEAResult::addBlockEffect(Effect E) {
  assert(E.Block);
  BlockEffects[E.Block].push_back(std::move(E));
}

void PEAResult::clearBlockEffects(ArrayRef<BasicBlock *> Blocks) {
  for (BasicBlock *BB : Blocks)
    BlockEffects.erase(BB);
}

bool PEAResult::hasOptimizationOpportunity() const {
  return VirtualizationDelta > 0 || AllocationDelta != 0 ||
         !BlockEffects.empty();
}
