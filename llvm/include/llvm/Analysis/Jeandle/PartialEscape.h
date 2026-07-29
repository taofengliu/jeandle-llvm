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
#include "llvm/ADT/STLFunctionalExtras.h"
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
class Function;
class GetElementPtrInst;
class Instruction;
class InvokeInst;
class LLVMContext;
class PHINode;
class raw_ostream;
class Type;
class Value;

namespace jeandle {

class AliasMap;
class Effect;
class EffectList;
class PEABlockState;
struct TransformContext;
namespace pea {
class FinalDeoptPoolBundlePlan;
}

// ===========================================================================
// 1.1  ObjectID and VirtualObject
// ===========================================================================

using ObjectID = unsigned;
static constexpr ObjectID InvalidObjectID = ~0u;

// Identity of a virtual monitorenter, carried per element of an
// ObjectState's live lock stack. EnterCall is the original
// jeandle.monitorenter call site (the effects-side anchor used when
// un-eliding the call on materialisation); BytecodeDepth is the lock-nesting
// ordering key at the enter site, reconstructed by an edge-sensitive CFG
// dataflow (see PartialEscapeAnalysis.cpp). It is the absolute dynamic nesting
// depth, including the inferred interpreter-held entry depth of an OSR root.
// The analyzer-side mirror struct LockEnter (PartialEscapeAnalysis.cpp)
// carries Call + BytecodeDepth. The function-wide dataflow makes the value
// stable across loop-fixpoint re-pushes.
struct MonitorIdRef {
  CallBase *EnterCall;
  uint32_t BytecodeDepth;

  bool operator==(const MonitorIdRef &O) const {
    return EnterCall == O.EnterCall && BytecodeDepth == O.BytecodeDepth;
  }
  bool operator!=(const MonitorIdRef &O) const { return !(*this == O); }
};

// Self-contained description of a monitorenter to re-emit at the materialize
// point. Graal analog: a synthetic MonitorEnterNode created at the
// CommitAllocationNode during lowering (DefaultJavaLoweringProvider
// finishAllocatedObjects), sorted ascending by lock depth. Jeandle captures
// this from the original enter call at ANALYSIS time because the lock model
// DELETES the original enter from IR — the transform cannot depend on
// the original call's lifetime. Callee is the jeandle.monitorenter_*
// function; NonReceiverArgs are operands 1..N (e.g. the BasicLock); the
// receiver (operand 0) is the effect's real receiver: OrigAlloc or
// SyntheticPhi.
// BytecodeDepth is the ascending re-emit sort key (Graal getLockDepth).
struct MaterializedLock {
  Function *Callee = nullptr;
  SmallVector<WeakTrackingVH, 2> NonReceiverArgs;
  uint32_t BytecodeDepth = 0;
};

// A lock re-emitted at a materialization point, tagged with its source
// MaterializeEffect so the transform can pick the right per-effect receiver
// for each lock when a cascade group's locks are merged and globally
// depth-sorted. SourceEffect is the per-effect receiver-lookup key (Jeandle's
// analog of Graal's `allocations[commit.getObjectIndex(monitorId)]`) —
// strictly more precise than an OrigAlloc key, which would be last-write-wins
// across per-pred materializations of the same object. The transform resolves
// the receiver via `MaterializedReceiverOf[SourceEffect]` (set once per effect
// to OrigAlloc or SyntheticPhi in applyMaterialize, read by the lock-cascade
// re-emit path). Materialization never spawns a per-pred invoke, so
// the per-effect key disambiguates cascade members without any fallback chain.
// See PEAResult::LockReplayBatches.
class MaterializeEffect;
struct MergedLock {
  Function *Callee = nullptr;
  SmallVector<WeakTrackingVH, 2> NonReceiverArgs;
  uint32_t BytecodeDepth = 0;
  const MaterializeEffect *SourceEffect = nullptr; // per-effect receiver key
  // Logical consumers whose effects contributed this one physical replay
  // operation. The transform emits the operation once; tracing emits one
  // association row per unique consumer using the same physical ordinal.
  SmallVector<const Value *, 2> LogicalEscapes;
};

// Final physical lock-replay batch consumed by the transform. EmitSite is the
// durable edge-normalized, pre-Pass1 insertion point (before any eager re-aim),
// and EmitterSeqNo identifies the tail MaterializeEffect that emits the
// complete globally depth-sorted list after every receiver has been recorded.
struct LockReplayBatch {
  Instruction *EmitSite = nullptr;
  uint32_t EmitterSeqNo = 0;
  uint32_t SourceID = 0;
  SmallVector<MergedLock, 4> Locks;
};

class VirtualObject {
public:
  enum ClassKind : uint8_t { Instance, Array };

  struct FieldDesc {
    int64_t Offset;
    Type *LLVMType;
    uint8_t ByteSize;
    bool IsReference;

    // Empty means an endpoint overflowed and overlap is unknown.
    std::optional<bool> overlaps(int64_t Off, uint8_t Size) const;
  };

private:
  const ObjectID ID;
  ClassKind Kind;

public:
  // The original `jeandle.new_instance`/`jeandle.new_array` invoke emitted by
  // the JDK abstract interpreter. This is Jeandle's analog of Graal's
  // VirtualObjectNode — the identity token that downstream IR uses reference.
  // LLVM-CONSTRAINED DIVERGENCE from Graal: Graal REPLACES the NewInstanceNode
  // with a VirtualObjectNode DURING analysis (tool.replaceWithVirtual) and
  // resolves uses through the point-sensitive `aliases` map; there is no
  // persistent "OrigAlloc". Jeandle's Analysis pass cannot mutate IR (LLVM's
  // Analysis/Transform split), so OrigAlloc persists as a real invoke until the
  // Transform, where an ordinary VO is either eliminated (NeverEscapes) or
  // KEPT and reused as the materialized value itself for PartiallyEscapes. The
  // synthetic Case-C form is documented below and uses SyntheticPhi instead.
  // The analysis
  // rewrites every VirtualRef to the referenced object's real identity during
  // prerequisite materialization (see processLoad/processStore/
  // resolveVirtualRef), so the transform only replays field stores and
  // re-emits locks onto the selected receiver. Ordinary receivers are
  // OrigAlloc; prepared Case-C receivers are SyntheticPhi.
  // Behaviorally equivalent: OrigAlloc's role is a pure identity token / the
  // single sound SSA materialized value, never a fresh allocation in the final
  // IR.
  // WeakTrackingVH: an atomic deopt-pool rewrite may clone the allocation
  // invoke and RAUW the original. The handle follows to the clone so
  // transform-time uses remain valid.
  WeakTrackingVH AllocationCall;

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
  // per-pred VOs in original PHI-incoming order. A proven-dead structural
  // incoming retains an InvalidObjectID slot until CFG cleanup removes it;
  // Unseen loop inputs are backfilled by the loop fixpoint. SyntheticPhi is
  // the merge-block PHINode the VO is aliased to. If the synthetic identity
  // later escapes, every real leaf allocation is retained at its original
  // allocation site and the complete current state is replayed once onto
  // SyntheticPhi. No allocation is created for the synthetic itself.
  bool IsSynthetic = false;
  SmallVector<ObjectID, 4> SyntheticSourceIDs;
  PHINode *SyntheticPhi = nullptr;

  // Defined out-of-line (PartialEscape.cpp): CallBase is incomplete here and
  // the WeakTrackingVH assignment needs the CallBase* → Value* conversion.
  VirtualObject(ObjectID id, ClassKind k, CallBase *alloc);

  ObjectID getID() const { return ID; }
  ClassKind getKind() const { return Kind; }
  bool isInstance() const { return Kind == Instance; }
  bool isArray() const { return Kind == Array; }

  int getOrCreateFieldIndex(int64_t Offset, Type *Ty, const DataLayout &DL);

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

  // Copy the structural (non-identity, non-synthetic) fields from O into *this.
  // Shared by duplicate() and PEAResult::createVirtualObject() so the field
  // list lives in exactly one place (avoids drift when a field is added).
  void copyStructuralFieldsFrom(const VirtualObject &O);

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
  // Valid when T is Scalar or MaterializedRef. WeakTrackingVH (NOT a raw
  // Value*): the snapshotted value may be RAUW'd during the transform — a
  // folded load RAUW'd to its replacement, or a safepoint call cloned by an
  // atomic deopt-pool rewrite — and a raw Value* would then dangle.
  // The handle follows the RAUW so the snapshot never dangles. A value
  // DELETED without replacement nulls the handle; the transform asserts
  // values are non-null and parented (or Constant/Argument) at use. Valid
  // when T is VirtualRef: the vo-id.
  WeakTrackingVH V;
  ObjectID Ref = InvalidObjectID;
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

  Value *getScalar() const {
    assert(isScalar());
    return V;
  }
  ObjectID getVirtualRef() const {
    assert(isVirtualRef());
    return Ref;
  }
  Value *getMaterialized() const {
    assert(isMaterializedRef());
    return V;
  }
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

public:
  // ObjectState carries ONLY the per-VO virtual/materialized flag, the live
  // lock stack, and (once materialized) the materialized pointer. Per-FIELD
  // state does NOT live here. Graal's ObjectState.entries is authoritative
  // because Graal propagates an ObjectState[] across the CFG inside
  // PartialEscapeBlockState; Jeandle cannot (LLVM's Analysis/Transform split +
  // SSA single-pass walk — see the STATE MODEL comment in
  // PartialEscapeAnalysis.cpp), so field values are tracked in the
  // analyzer-wide FieldStates DenseMap keyed by (ObjectID, byte-offset), and
  // ObjectState intentionally carries no field-storage member.
  // Copy/move/assign/dtor are implicitly generated: ObjectState is a plain bag
  // of value members, so PEABlockState's array-level copy-on-write (which
  // deep-copies every slot) needs no per-slot sharing annotation here.

  StateKind getKind() const { return Kind; }
  bool isVirtual() const { return Kind == Virtual; }
  bool isMaterialized() const { return Kind == Materialized; }

  Value *getMaterializedValue() const {
    assert(isMaterialized() && MaterializedValue);
    return MaterializedValue;
  }
  bool hasLocks() const { return !Locks.empty(); }

  // Graal analog: ObjectState.escape(ValueNode materialized). This is the
  // pure virtual->materialized STATE FLIP only — it does NOT emit a
  // Materialize effect or build an allocation invoke (that is the caller's
  // job, e.g. materializeAt / ensureMaterialized). Named `escape` after Graal
  // to avoid colliding with the real-materialization method family below.
  void escape(Value *Ptr) {
    assert(isVirtual());
    assert(Ptr);
    Kind = Materialized;
    MaterializedValue = Ptr;
    // Graal ObjectState.escape retains the lock state across the
    // virtual->materialized flip — the materialized state still reads locks
    // for re-emit and the strict-lock cascade. We match that: escape() does
    // NOT clear Locks. Callers that need the live lock state dropped (the
    // live-path materializeAt, via ClearLockState -> clearLocks) do so
    // explicitly before flipping; the analyzer-side LiveLockEnters/LockCounts
    // maps remain the cross-block authority, so any retained on-VO locks are
    // informational only.
  }

  void addLock(MonitorIdRef M) {
    assert(isVirtual());
    // Graal ObjectState.addLock guarantees strictly descending depth on the
    // head: the new (innermost) lock's depth is strictly greater than the
    // previous head. Jeandle's vector runs front=min(outermost)..back=max
    // (innermost), so the invariant is that the pushed depth strictly exceeds
    // the current back. Mirrors Graal's GraalError.guarantee; the lock
    // cascade (ensureMaterialized / materializeVirtualLocksBefore) relies on
    // front()=min / back()=max.
    assert(Locks.empty() || M.BytecodeDepth > Locks.back().BytecodeDepth);
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
};

// ===========================================================================
// 1.4  PEABlockState
// ===========================================================================

class PEABlockState {
private:
  // ObjectStates is shared via std::shared_ptr and is copy-on-write: a mutator
  // calls getArrayForModification(), which deep-copies the array when it is
  // currently shared. Sharing is read directly from ObjectStates.use_count()
  // (== 1 ⇒ sole owner, mutate in place; > 1 ⇒ clone first). Because every
  // shared copy/move/destroy of a PEABlockState is just the shared_ptr doing
  // the same to its control block, use_count() is an exact sharer count and no
  // parallel manual refcount is needed. copy/move/assign/dtor are therefore
  // implicitly generated (rule of zero).
  std::shared_ptr<SmallVector<std::optional<ObjectState>, 8>> ObjectStates;

public:
  PEABlockState();

  void addObject(ObjectID ID, ObjectState State);

  bool hasObjectState(ObjectID ID) const;
  const ObjectState &getObjectState(ObjectID ID) const;
  ObjectState &getObjectStateForModification(ObjectID ID);
  const ObjectState *getObjectStateOptional(ObjectID ID) const;

  unsigned getStateCount() const {
    return ObjectStates ? static_cast<unsigned>(ObjectStates->size()) : 0u;
  }
  // CFG liveness is target-specific rather than part of the shared virtual
  // object state. The analyzer's BlockExitInfo and EdgeContribution classify
  // structural predecessor inputs as Unseen, Dead, or Live.

private:
  SmallVector<std::optional<ObjectState>, 8> *getArrayForModification();
};

// ===========================================================================
// 1.5  AliasMap
// ===========================================================================

class AliasMap {
  DenseMap<Value *, ObjectID> VirtualAliases;
  DenseSet<Value *> WholeObjectVirtualAliases;
  DenseMap<Value *, Value *> ScalarAliases;
  DenseSet<Instruction *> HasVirtualInputs;

public:
  void addVirtualAlias(Value *V, ObjectID ID, bool IsWholeObject);
  void addScalarAlias(Value *V, Value *Replacement);
  void resetAlias(Value *V);

  std::optional<ObjectID> getVirtualAlias(Value *V) const;
  bool isWholeObjectVirtualAlias(Value *V) const {
    return WholeObjectVirtualAliases.count(V);
  }
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
// 1.6  Effects
//
// Graal's PEA models an IR mutation as an `Effect` (EffectList.java):
//   - abstract base `Effect` with `isVisible()`, `isCfgKill()`,
//     `apply(graph, obsoleteNodes)`, `format()`;
//   - concrete effects are anonymous subclasses, one per mutation kind,
//     categorized by `isCfgKill()` (drives the two-pass apply) and
//     `isVisible()` (logging only);
//   - stored per-block in a `GraphEffectList` (EffectList subclass) and
//     applied in two passes — non-cfgKill first, cfgKill second — driven
//     entirely by `isCfgKill()`.
//
// Jeandle mirrors this shape with three IR-form-induced adaptations:
//   1. LLVM's Analysis/Transform split forbids mutating IR during analysis,
//      so Jeandle's effects are serializable DATA RECORDS (named subclasses
//      with fields), not anonymous closures capturing graph nodes. They are
//      produced by the analysis pass, marshalled in `PEAResult`, and consumed
//      by a separate transform pass.
//   2. `apply(TransformContext&)` is the Jeandle form of Graal's
//      `apply(graph, obsoleteNodes)` — `TransformContext` bundles the
//      Function and the shared per-apply maps (defined in
//      PartialEscapeTransform.cpp).
//   3. A safepoint's complete deoptimization object pool is one atomic phase
//      between ordinary SSA rewrites and cfg-kill effects. LLVM operand
//      bundles have no independently mutable FrameState/ObjectState graph, so
//      exposing per-object bundle edits would create observable intermediate
//      states that do not exist in Graal.
// ===========================================================================

// Jeandle adaptation of Graal's `EffectList.Effect`. Abstract base for every
// recorded IR mutation.
class Effect {
public:
  enum class Phase : uint8_t { Ordinary, DeoptPool, CfgKill };

  enum class Kind : uint8_t {
    ReplaceLoad,
    ReplaceCall,
    EliminateStore,
    EliminateAllocation,
    Materialize,
    CreatePHI,
    // Atomically replace one safepoint's complete deoptimization object pool.
    // Unlike ordinary effects it has no single virtual-object mutation owner.
    RewriteDeoptPool,
  };

  // --- common fields (read by commit/dropEffectsFor/the transform) ---
  // Block is the effect's semantic insertion block. During edge
  // normalization a MaterializeEffect may be retargeted to a newly split edge
  // block while its owning unique_ptr remains in the original BlockEffects
  // bucket; transform application order still comes from that stable bucket,
  // and the physical insertion point comes from InsertBefore.
  BasicBlock *Block = nullptr;
  // IR-FORM DIVERGENCE from Graal: Graal applies effects in pure list-order
  // (its per-block EffectList is append-only; loop headers use
  // insertAll(...,0)). Jeandle marshals effects into a per-block map and
  // re-sorts at apply time, so it needs an explicit ordering key. The
  // deferred-CreatePHI trick (emit at SeqNo=0, reassign a fresh nextSeqNo()
  // at drain) is load-bearing for the ordinary self-loop back-edge ordering
  // invariant (CreatePHI sorts after merge-time per-pred Materialize in the
  // same block).
  // TODO(list-order): adopting Graal's pure list-order would require reworking
  // merge/loop emission; SeqNo + the deferred-CreatePHI trick covers ordering
  // meanwhile.
  uint32_t SeqNo = 0;

private:
  // The one virtual object whose ordinary IR mutation this effect performs.
  // A deopt-pool rewrite spans zero or more objects and has no mutation owner.
  ObjectID MutationOwner = InvalidObjectID;

public:
  bool hasMutationOwner() const { return MutationOwner != InvalidObjectID; }
  ObjectID getMutationOwner() const { return MutationOwner; }
  void setMutationOwner(ObjectID ID) {
    assert(ID != InvalidObjectID);
    MutationOwner = ID;
  }

  virtual ~Effect() = default;

  // Graal's non-cfgKill/cfgKill ordering with an LLVM-specific atomic
  // deopt-pool phase inserted between them. This phase is not the same as
  // "structurally rewrites the CFG": Materialize and CreatePHI run in the
  // ordinary phase.
  virtual Phase getPhase() const { return Phase::Ordinary; }

  virtual Kind getKind() const = 0;

  bool hasValidMutationOwner() const {
    return getKind() == Kind::RewriteDeoptPool ? !hasMutationOwner()
                                               : hasMutationOwner();
  }

  // The IR instruction this effect rewrites/erases, or null for effects that
  // have no single target (CreatePHI). Read generically by the analyzer-side
  // scans (processBlock tail, processBlockPhis identity check, commit).
  virtual Instruction *getTarget() const { return nullptr; }

  // Graal apply(graph, obsoleteNodes). Mutates Ctx.F's IR. Defined in
  // PartialEscapeTransform.cpp.
  virtual void apply(TransformContext &Ctx) = 0;

  // Graal format()/toString(). Drives the -jeandle-trace-pea per-effect line.
  // Non-pure: the kind name, mutation owner, block, and target are all
  // reachable via the base API, so one shared definition suffices.
  virtual void dump(raw_ostream &OS) const;

  // Deep copy for the loop-fixpoint snapshot (takeLoopSnapshot/
  // restoreLoopSnapshot read the snapshot across multiple iterations, so they
  // need a copy; EffectList is move-only). IR-form divergence: Graal
  // backtracks by truncating its EffectList (size=0); Jeandle's per-block map
  // snapshot needs a real deep copy.
  virtual std::unique_ptr<Effect> clone() const = 0;
};

// Fold a load from a virtual object's field to the tracked scalar value (or a
// synthesized coercion / default). Non-cfgKill. Graal analog: replaceAtUsages.
class ReplaceLoadEffect : public Effect {
public:
  // WeakTrackingVH follows RAUW, including a safepoint clone performed by the
  // atomic deopt-pool phase; null on deletion without replacement.
  WeakTrackingVH Target;
  WeakTrackingVH Replacement;

  Kind getKind() const override { return Kind::ReplaceLoad; }
  static bool classof(const Effect *E) {
    return E->getKind() == Kind::ReplaceLoad;
  }
  Instruction *getTarget() const override {
    return dyn_cast_or_null<Instruction>((Value *)Target);
  }
  void apply(TransformContext &Ctx) override;
  std::unique_ptr<Effect> clone() const override {
    return std::make_unique<ReplaceLoadEffect>(*this);
  }
};

// Fold a `jeandle.*` JavaOp against a virtual receiver to a constant / delete.
// Non-cfgKill. Graal analog: replaceAtUsages / deleteNode.
class ReplaceCallEffect : public Effect {
public:
  // WeakTrackingVH: see ReplaceLoadEffect.
  WeakTrackingVH Target;
  WeakTrackingVH Replacement;
  // When >= 0, the transform ignores `Replacement` and instead builds a GC-safe
  // oop-handle load (see createConstOopLoad) of the constant Java oop named by
  // this id, inserting it before `Target` and using it as the replacement. Used
  // by foldGetClass: the analyzer cannot build the load itself without creating
  // a module-level GlobalVariable during analysis, so it defers to the
  // transform. -1 means "use Replacement as-is" (the normal path).
  int OopHandleId = -1;

  Kind getKind() const override { return Kind::ReplaceCall; }
  static bool classof(const Effect *E) {
    return E->getKind() == Kind::ReplaceCall;
  }
  Instruction *getTarget() const override {
    return dyn_cast_or_null<Instruction>((Value *)Target);
  }
  void apply(TransformContext &Ctx) override;
  std::unique_ptr<Effect> clone() const override {
    return std::make_unique<ReplaceCallEffect>(*this);
  }
};

// Remove a store into a virtual object's field (value tracked in FieldStates).
// Non-cfgKill. Graal analog: deleteNode (FixedWithNextNode).
class EliminateStoreEffect : public Effect {
public:
  Instruction *Target = nullptr;

  Kind getKind() const override { return Kind::EliminateStore; }
  static bool classof(const Effect *E) {
    return E->getKind() == Kind::EliminateStore;
  }
  Instruction *getTarget() const override { return Target; }
  void apply(TransformContext &Ctx) override;
  std::unique_ptr<Effect> clone() const override {
    return std::make_unique<EliminateStoreEffect>(*this);
  }
};

// Rewrite the original allocation invoke into an unconditional branch (dropping
// the unwind edge) or erase a call alloc. Applied in the cfg-kill phase.
// Graal analog: deleteNode (WithExceptionNode) / killIfBranch.
class EliminateAllocationEffect : public Effect {
public:
  // WeakTrackingVH: the allocation may be cloned by the deopt-pool phase; the
  // handle follows so the cfg-kill phase erases the clone.
  WeakTrackingVH Target;

  Kind getKind() const override { return Kind::EliminateAllocation; }
  static bool classof(const Effect *E) {
    return E->getKind() == Kind::EliminateAllocation;
  }
  Phase getPhase() const override { return Phase::CfgKill; }
  Instruction *getTarget() const override {
    return dyn_cast_or_null<Instruction>((Value *)Target);
  }
  void apply(TransformContext &Ctx) override;
  std::unique_ptr<Effect> clone() const override {
    return std::make_unique<EliminateAllocationEffect>(*this);
  }
};

// Materialize an escape point by replaying tracked field stores and re-emitting
// surviving monitorenters onto the real identity. For an ordinary VO this is
// OrigAlloc (VObj.AllocationCall), which dominates every escape point. For a
// prepared synthetic Case-C VO, it is SyntheticPhi. Neither form emits a fresh
// allocation invoke. The receiver is recorded in
// `MaterializedReceiverOf[&E]`.
// Ordinary phase.
// Graal analog: the one `Effect("materializeBefore")` appended by
// PartialEscapeBlockState.materializeBefore.
class MaterializeEffect : public Effect {
public:
  static constexpr uint32_t InvalidPlanID = ~uint32_t{0};

  // Per-offset snapshot of a virtual object's field values at a
  // materialization point.
  struct FieldEntry {
    int64_t Offset;
    FieldValue Value;
  };

  // WeakTrackingVH so erasing the insertion-point instruction auto-nulls the
  // handle. applyMaterialize ASSERTS this is non-null — the eager-update hook
  // relocateDependentMaterializes re-aims every dependent Materialize's
  // InsertBefore to the dying instruction's in-block successor BEFORE the
  // instruction is erased (the ordinary phase runs effects in RPO, so an erase
  // in an earlier block re-aims dependents before they reach apply — see the
  // InsertBeforeDependents pre-scan built in the TransformContext). The
  // handle is NOT recomputed at apply time; WeakTrackingVH is only there to
  // fail loudly rather than dangling.
  WeakTrackingVH InsertBefore;
  // Replay receiver: OrigAlloc for an ordinary VO, SyntheticPhi for a
  // synthetic Case-C VO. WeakTrackingVH follows any safepoint clone.
  WeakTrackingVH Target;
  SmallVector<FieldEntry, 8> FieldEntries;
  // Surviving (unbalanced) monitorenters to re-emit at the materialize point
  // (Graal: synthetic MonitorEnterNodes at the CommitAllocationNode), sorted
  // ascending by BytecodeDepth.
  SmallVector<MaterializedLock, 2> Locks;
  // Stable provenance for the final lock-replay plan. LogicalEscape identifies
  // the merge/consumer shared by alternative predecessor plans; ReplaySource
  // identifies the concrete predecessor/path that physically emits this
  // effect. computeEscapePointLocks converts these analysis-owned anchors to
  // deterministic numeric IDs before the transform mutates IR.
  Value *LogicalEscape = nullptr;
  BasicBlock *ReplaySource = nullptr;
  // The merge reached by a merge-driven incoming edge.  When ReplaySource
  // has another distinct successor, the transform splits Source->Target and
  // moves every replay operation to that edge before building batches. Null
  // for live-path materializations and true block-end drains.
  BasicBlock *ReplayTarget = nullptr;
  // Recursive field prerequisites and strict-lock cascade members share one
  // plan ID. Final eligibility treats every surviving plan as an atomic
  // component: if any member is ineligible, every member's effects are
  // discarded before transform application.
  uint32_t PlanID = InvalidPlanID;
  // Incoming-edge replay uses the effect's real receiver. Field and lock replay
  // retain edge provenance, and per-pred receiver identity survives via
  // MaterializedReceiverOf[SourceEffect].

  Kind getKind() const override { return Kind::Materialize; }
  static bool classof(const Effect *E) {
    return E->getKind() == Kind::Materialize;
  }
  Instruction *getTarget() const override {
    return dyn_cast_or_null<Instruction>((Value *)Target);
  }
  void apply(TransformContext &Ctx) override;
  std::unique_ptr<Effect> clone() const override {
    return std::make_unique<MaterializeEffect>(*this);
  }
  // Block and InsertBefore are assigned at emission. Before apply, the
  // transform re-aims both to a dedicated edge block when ReplaySource has
  // another distinct successor; the replay receiver remains unchanged and
  // dominates the normalized insertion point by SSA.
  // Defined out-of-line: assigning to the WeakTrackingVH needs Instruction to
  // be a complete type (Instruction* → Value* derived-to-base), which it is
  // not in this header.
  void setInsertBefore(Instruction *I);
};

// Insert an analyzer-built unparented PHINode at a merge block and wire its
// incomings. Ordinary phase. Graal analog: addFloatingNode + setPhiInput
// (initializePhiInput).
//
// CreatePHIEffects are field-value PHIs that merge a per-offset field value
// across predecessors or around a loop. They are emitted by mergeFieldStates
// and synthesizeCaseC.
class CreatePHIEffect : public Effect {
public:
  Type *PHIType = nullptr;
  int64_t FieldOffset = 0;
  // WeakTrackingVH: an incoming value may be a call result whose call is
  // cloned by the deopt-pool phase; the handle follows the RAUW.
  SmallVector<WeakTrackingVH, 4> PHIIncomingValues;
  SmallVector<BasicBlock *, 4> PHIIncomingBlocks;
  PHINode *PhiInst = nullptr;

  Kind getKind() const override { return Kind::CreatePHI; }
  static bool classof(const Effect *E) {
    return E->getKind() == Kind::CreatePHI;
  }
  void apply(TransformContext &Ctx) override;
  std::unique_ptr<Effect> clone() const override {
    return std::make_unique<CreatePHIEffect>(*this);
  }
};

// One immutable, complete deopt object-pool plan for a safepoint. The plan
// owns every output token and every exact current-object source occurrence;
// transform application therefore performs one checked rebuild rather than
// composing per-object edits.
class RewriteDeoptPoolEffect : public Effect {
public:
  RewriteDeoptPoolEffect(
      CallBase *Safepoint,
      std::shared_ptr<const pea::FinalDeoptPoolBundlePlan> Plan);

  Kind getKind() const override { return Kind::RewriteDeoptPool; }
  static bool classof(const Effect *E) {
    return E->getKind() == Kind::RewriteDeoptPool;
  }
  Phase getPhase() const override { return Phase::DeoptPool; }
  Instruction *getTarget() const override;

  CallBase *getSafepointKey() const { return SafepointKey; }
  bool hasPlan() const { return static_cast<bool>(Plan); }
  const pea::FinalDeoptPoolBundlePlan &getPlan() const {
    assert(Plan && "deopt-pool effect has no plan");
    return *Plan;
  }
  bool coversExactOccurrence(unsigned SemanticCell, ObjectID ID) const;
  bool acceptsCurrentMember(ObjectID ID) const;

  void apply(TransformContext &Ctx) override;
  std::unique_ptr<Effect> clone() const override {
    return std::make_unique<RewriteDeoptPoolEffect>(*this);
  }

private:
  // Stable analysis-time identity. It is compared and used as a map key only,
  // never dereferenced after SafepointVH stops denoting the same value.
  CallBase *const SafepointKey;
  WeakTrackingVH SafepointVH;
  const std::shared_ptr<const pea::FinalDeoptPoolBundlePlan> Plan;
};

// Jeandle adaptation of Graal's `EffectList` / `GraphEffectList`. Append-only
// list of `unique_ptr<Effect>` (mirrors Graal's `Effect[]` of references).
// Move-only: the loop-fixpoint snapshot needs a copy, taken via `clone()`.
class EffectList {
  SmallVector<std::unique_ptr<Effect>, 16> Effects;

public:
  EffectList() = default;
  EffectList(EffectList &&) = default;
  EffectList &operator=(EffectList &&) = default;
  EffectList(const EffectList &) = delete;
  EffectList &operator=(const EffectList &) = delete;

  // Graal add(): append one effect at the tail.
  void add(std::unique_ptr<Effect> E) { Effects.push_back(std::move(E)); }
  // Graal addAll(): append all of Other at the tail (move-merge, clear Other).
  void addAll(EffectList &Other) {
    for (auto &E : Other.Effects)
      Effects.push_back(std::move(E));
    Other.Effects.clear();
  }
  // Graal clear(): size=0, retain capacity for backtracking reuse.
  void clear() { Effects.clear(); }
  bool empty() const { return Effects.empty(); }
  size_t size() const { return Effects.size(); }
  Effect &operator[](size_t I) { return *Effects[I]; }
  const Effect &operator[](size_t I) const { return *Effects[I]; }

  // IR-form extension: remove and return ownership of element I. Used by
  // processBlock's post-merge PendingMergePhis drain to move synthesized
  // Case-B PHI effects into BlockEffects in SeqNo order.
  std::unique_ptr<Effect> spliceOut(size_t I) {
    auto E = std::move(Effects[I]);
    Effects.erase(Effects.begin() + I);
    return E;
  }
  // IR-form extension: drop every effect matching Pred (dropEffectsFor).
  void eraseIf(function_ref<bool(const Effect &)> Pred) {
    SmallVector<std::unique_ptr<Effect>, 16> Kept;
    for (auto &E : Effects)
      if (!Pred(*E))
        Kept.push_back(std::move(E));
    Effects = std::move(Kept);
  }
  // Deep copy for the loop-fixpoint snapshot.
  EffectList clone() const {
    EffectList C;
    for (const auto &E : Effects)
      C.Effects.push_back(E->clone());
    return C;
  }

  // Apply every effect in Phase, in SeqNo order (Jeandle's substitute for
  // Graal list-order — see Effect::SeqNo). Defined in
  // PartialEscapeTransform.cpp.
  void apply(TransformContext &Ctx, Effect::Phase Phase);

  // Range-for yielding Effect& / const Effect& (the analyzer-side scans iterate
  // by reference; ownership stays with the list).
  struct Iterator {
    SmallVectorImpl<std::unique_ptr<Effect>>::iterator It;
    Effect &operator*() const { return **It; }
    Iterator &operator++() {
      ++It;
      return *this;
    }
    bool operator==(const Iterator &O) const { return It == O.It; }
    bool operator!=(const Iterator &O) const { return It != O.It; }
  };
  struct ConstIterator {
    SmallVectorImpl<std::unique_ptr<Effect>>::const_iterator It;
    const Effect &operator*() const { return **It; }
    ConstIterator &operator++() {
      ++It;
      return *this;
    }
    bool operator==(const ConstIterator &O) const { return It == O.It; }
    bool operator!=(const ConstIterator &O) const { return It != O.It; }
  };
  Iterator begin() { return {Effects.begin()}; }
  Iterator end() { return {Effects.end()}; }
  ConstIterator begin() const { return {Effects.begin()}; }
  ConstIterator end() const { return {Effects.end()}; }
};

// ===========================================================================
// 1.7  PEAResult
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

  DenseMap<BasicBlock *, EffectList> BlockEffects;

  // Final physical lock-replay batches, globally depth-sorted ascending by
  // BytecodeDepth. Graal flattens every lock materialized at one point into a
  // single CommitAllocationNode and lowers them globally depth-sorted with a
  // strict-increase guarantee (DefaultJavaLoweringProvider). Jeandle's per-VO
  // MaterializeEffect model otherwise would re-emit per-effect, which
  // mis-orders re-entrant interleaved cascades (e.g. [a@0,b@1,a@2,c@3]
  // re-emitting as 0,2,1,3).
  //
  // The transform's tail-effect path emits an escape point's merged list once,
  // from the highest-SeqNo MaterializeEffect among those SHARING the SAME
  // edge-normalized, pre-Pass1 InsertBefore pointer (the locked re-emit loop
  // in applyMaterialize, gated on E.SeqNo == Batch.EmitterSeqNo). Each lock's
  // receiver is resolved via MaterializedReceiverOf[ML.SourceEffect], asserted
  // non-empty — NO fallback chain.
  //
  // GROUPING RULE: the transform first normalizes each multi-successor
  // ReplaySource->ReplayTarget edge to a dedicated edge block, re-aiming all
  // materialize effects and deferred PHI incoming blocks together. It then
  // groups purely by the normalized MaterializeEffect::InsertBefore pointer.
  // A lock-carrying site with one effect receives a one-entry physical batch;
  // lockless sites replay only their fields and consume no lock-batch ID.
  //
  // All materializations for one normalized incoming edge share its block
  // terminator and therefore one physical batch. Distinct edge targets from
  // the same original source have distinct sites and never mix side effects.
  // Logical consumers are associations on each physical MergedLock, not
  // properties of the whole batch: lockless effects contribute no association,
  // while locks that dedup within one edge are emitted once and traced once per
  // association with the same physical ordinal.
  //
  // CROSS-PH per-pred cascades — multiple per-pred materializes from different
  // PHs feeding the same escape point or merge — remain separate physical
  // batches, each with its own source provenance. Cross-object interleaved-lock
  // ordering is therefore not combined across mutually exclusive paths.
  //
  // OrigInsertBefore (captured by the TransformContext after edge normalization
  // and before the ordinary phase) records InsertBefore before any
  // eager-update re-aim by
  // relocateDependentMaterializes; the lock key is exactly this captured
  // normalized pointer (looked up via OrigInsertBefore.lookup(&E) — a re-aimed
  // E.InsertBefore could otherwise miss the final physical batch).
  //
  // Populated by computeEscapePointLocks(), called once before the ordinary
  // phase from
  // the TransformContext setup in run().
  SmallVector<LockReplayBatch, 4> LockReplayBatches;
  DenseMap<Instruction *, unsigned> LockReplayBatchForSite;
  void computeEscapePointLocks();
  const LockReplayBatch *getLockReplayBatch(Instruction *EmitSite) const {
    auto It = LockReplayBatchForSite.find(EmitSite);
    if (It == LockReplayBatchForSite.end())
      return nullptr;
    return &LockReplayBatches[It->second];
  }

  int VirtualizationDelta = 0;
  int AllocationDelta = 0;
  uint32_t NextSeqNo = 0;
  // Analysis proved that at least one structural CFG edge/terminator is dead.
  // The transform must run its terminator and unreachable-block cleanup even
  // when applying the surviving effects does not otherwise mutate the IR.
  bool NeedsCFGCleanup = false;
  // Blocks excluded by the final, validated CFG-deadness plan. Uses in these
  // blocks are removed by the transform's unreachable-block cleanup and are
  // not surviving semantic uses of a NeverEscapes allocation.
  DenseSet<BasicBlock *> FinalDeadBlocks;

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

  // PHI nodes synthesized by mergeStates at ANY in-loop merge block (a loop
  // header OR a non-header block inside a loop), stored separately from
  // OwnedPhis because they must survive rollback within the loop-fixpoint
  // iteration. Each iteration re-runs mergeStates(Header) and would otherwise
  // allocate a fresh PHI per (BB, ID, offset), producing fresh Value* pointers
  // in FieldStates so the convergence check could never compare equal. The
  // analyzer's LoopFieldPhiCache (keyed on (BB, ID, offset)) returns the same
  // PHI across iterations; only the per-iteration CreatePHI Effect is rebuilt.
  // Non-header in-loop merges are included because restoreLoopSnapshot
  // preserves BlockExits[BB] for every loop block across iterations, so any
  // Value* reachable from a preserved BlockExits[BB] must outlive rollback —
  // were such a PHI to land in OwnedPhis (which IS truncated), the preserved
  // BlockExits[BB] would reference a deleted PHI. Lifecycle is identical to
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

  void addBlockEffect(std::unique_ptr<Effect> E);
  void publishEffectTrace() const;
  bool hasUniqueEffectSequenceNumbers() const;
  bool hasLegalMaterializationAtomicTypes(const DataLayout &DL) const;

  bool hasOptimizationOpportunity() const;

  // Whether CurrentID's runtime identity can denote SourceID. Ordinary
  // identities represent only themselves. A synthetic Case-C identity
  // represents the transitive closure of its SyntheticSourceIDs. Invalid and
  // out-of-range IDs are rejected, and cycles in nested/loop-carried
  // synthetic graphs are bounded.
  bool currentIdentityRepresentsSource(ObjectID CurrentID,
                                       ObjectID SourceID) const;

  // Truncate OwnedPhis/OwnedInsts to the given marks, deleting each trailing
  // entry that is still unparented (the transform only inserts these into a
  // BasicBlock later, so anything added during a discarded merge iteration or
  // loop-iteration is unparented at rollback time). OwnedLoopFieldPhis is
  // intentionally NOT touched — it is the per-loop PHI cache whose stability
  // across fixpoint iterations is the whole point. Shared by deleteOwnedSince
  // (per-merge rollback) and restoreLoopSnapshot (loop-iteration rollback) so
  // the WeakTrackingVH -> dyn_cast -> delete/deleteValue -> pop_back logic
  // lives in exactly one place.
  void truncateOwnedTo(size_t PhisMark, size_t InstsMark);
};

} // namespace jeandle
} // namespace llvm

#endif // LLVM_ANALYSIS_JEANDLE_PARTIALESCAPE_H
