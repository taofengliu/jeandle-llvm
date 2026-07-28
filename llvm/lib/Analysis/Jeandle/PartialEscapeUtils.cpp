//===- PartialEscapeUtils.cpp - PEA helper implementations ------*- C++ -*-===//
//
// Copyright (c) 2026, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Pure helpers used by both the analysis and the transform pass.  No state.
// See PartialEscapeUtils.h for the interface.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/Jeandle/PartialEscapeUtils.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Jeandle/JavaType.h"
#include "llvm/IR/Jeandle/Metadata.h"
#include "llvm/IR/Jeandle/VMCallback.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/CheckedArithmetic.h"
#include "llvm/Support/MathExtras.h"

#include <limits>

using namespace llvm;

namespace llvm::jeandle::pea {

// ===========================================================================
// Callee-name predicates
// ===========================================================================

bool isJeandleCallNamed(const CallBase *CB, StringRef Name) {
  if (!CB)
    return false;
  // Reject inline asm and indirect calls.
  if (CB->isInlineAsm())
    return false;
  const Function *F = CB->getCalledFunction();
  if (!F)
    return false;
  return F->getName() == Name;
}

bool isJeandleNewInstance(const CallBase *CB) {
  return isJeandleCallNamed(CB, "jeandle.new_instance");
}

bool isJeandleNewArray(const CallBase *CB) {
  return isJeandleCallNamed(CB, "jeandle.new_array");
}

bool isJeandleAllocation(const CallBase *CB) {
  return isJeandleNewInstance(CB) || isJeandleNewArray(CB);
}

bool isJeandleArrayLength(const CallBase *CB) {
  return isJeandleCallNamed(CB, "jeandle.arraylength");
}

bool isJeandleLoadKlass(const CallBase *CB) {
  return isJeandleCallNamed(CB, "jeandle.load_klass");
}

bool isJeandleGetClass(const CallBase *CB) {
  return isJeandleCallNamed(CB, "jeandle.get_class");
}

bool isJeandleCheckCast(const CallBase *CB) {
  return isJeandleCallNamed(CB, "jeandle.checkcast");
}

bool isJeandleInstanceOf(const CallBase *CB) {
  return isJeandleCallNamed(CB, "jeandle.instanceof");
}

bool isJeandleCheckInstanceOf(const CallBase *CB) {
  return isJeandleCallNamed(CB, "jeandle.check_instanceof");
}

bool isJeandleCheckIfValueBased(const CallBase *CB) {
  return isJeandleCallNamed(CB, "jeandle.check_if_value_based");
}

bool isJeandleArrayStoreCheck(const CallBase *CB) {
  return isJeandleCallNamed(CB, "jeandle.array_store_check");
}

bool isJeandlePostBarrier(const CallBase *CB) {
  return isJeandleCallNamed(CB, "jeandle.post_barrier");
}

bool isJeandleMonitorEnter(const CallBase *CB) {
  return isJeandleCallNamed(CB, "jeandle.monitorenter_with_monitor_lock") ||
         isJeandleCallNamed(CB, "jeandle.monitorenter_with_thin_lock") ||
         isJeandleCallNamed(CB, "jeandle.monitorenter_with_lightweight_lock");
}

bool isJeandleMonitorExit(const CallBase *CB) {
  return isJeandleCallNamed(CB, "jeandle.monitorexit_with_monitor_lock") ||
         isJeandleCallNamed(CB, "jeandle.monitorexit_with_thin_lock") ||
         isJeandleCallNamed(CB, "jeandle.monitorexit_with_lightweight_lock");
}

bool isJeandleRegisterFinalizerIfNeeded(const CallBase *CB) {
  return isJeandleCallNamed(CB, "jeandle.register_finalizer_if_needed");
}

bool isPEAHandledNonEscapingIntrinsic(const IntrinsicInst *II) {
  if (!II)
    return false;
  switch (II->getIntrinsicID()) {
  case Intrinsic::assume:
  case Intrinsic::lifetime_start:
  case Intrinsic::lifetime_end:
  case Intrinsic::invariant_start:
  case Intrinsic::invariant_end:
  case Intrinsic::experimental_noalias_scope_decl:
  case Intrinsic::dbg_declare:
  case Intrinsic::dbg_value:
  case Intrinsic::dbg_label:
  case Intrinsic::donothing:
  case Intrinsic::sideeffect:
  case Intrinsic::var_annotation:
  case Intrinsic::is_constant:
  case Intrinsic::expect:
  case Intrinsic::expect_with_probability:
  case Intrinsic::allow_runtime_check:
  case Intrinsic::allow_ubsan_check:
  case Intrinsic::launder_invariant_group:
  case Intrinsic::strip_invariant_group:
  case Intrinsic::ptr_annotation:
    return true;
  default:
    return false;
  }
}

// ===========================================================================
// Type / klass helpers
// ===========================================================================

std::optional<JBasicType> elementTypeForArrayKlass(uintptr_t ArrayKlass) {
  if (ArrayKlass == 0)
    return std::nullopt;
  const jeandle::VMCallbacks *CB = jeandle::getVMCallbacks();
  if (!CB || !CB->ElementBasicTypeOfArrayKlass)
    return std::nullopt;
  int Raw = CB->ElementBasicTypeOfArrayKlass(ArrayKlass);
  if (Raw < 0 || Raw >= static_cast<int>(JBasicType::Count))
    return std::nullopt;
  return static_cast<JBasicType>(Raw);
}

Type *llvmElementTypeFor(JBasicType Kind, LLVMContext &Ctx) {
  switch (Kind) {
  case JBasicType::Boolean:
    return Type::getInt1Ty(Ctx);
  case JBasicType::Byte:
    return Type::getInt8Ty(Ctx);
  case JBasicType::Char:
    return Type::getInt16Ty(Ctx);
  case JBasicType::Short:
    return Type::getInt16Ty(Ctx);
  case JBasicType::Int:
    return Type::getInt32Ty(Ctx);
  case JBasicType::Long:
    return Type::getInt64Ty(Ctx);
  case JBasicType::Float:
    return Type::getFloatTy(Ctx);
  case JBasicType::Double:
    return Type::getDoubleTy(Ctx);
  case JBasicType::Object:
    return PointerType::get(Ctx, jeandle::AddrSpace::JavaHeapAddrSpace);
  case JBasicType::Count:
    return nullptr;
  }
  return nullptr;
}

bool isLegalMaterializationAtomicType(Type *Ty, const DataLayout &DL) {
  if (!Ty || !Ty->isSized())
    return false;

  Type *ScalarTy = Ty->getScalarType();
  if (!ScalarTy->isIntOrPtrTy() && !ScalarTy->isFloatingPointTy())
    return false;

  TypeSize Bits = DL.getTypeSizeInBits(Ty);
  if (Bits.isScalable())
    return false;
  uint64_t FixedBits = Bits.getFixedValue();
  return FixedBits >= 8 && isPowerOf2_64(FixedBits);
}

std::optional<int64_t> checkedOffsetAdd(int64_t LHS, int64_t RHS) {
  return checkedAdd(LHS, RHS);
}

std::optional<int64_t> checkedOffsetSub(int64_t LHS, int64_t RHS) {
  return checkedSub(LHS, RHS);
}

std::optional<int64_t> checkedArrayElementOffset(int64_t Base, int64_t Index,
                                                 uint64_t Scale) {
  if (Scale > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
    return std::nullopt;
  return checkedMulAdd(Index, static_cast<int64_t>(Scale), Base);
}

bool isUsableFieldOffset(int64_t Offset) {
  return Offset != DenseMapInfo<int64_t>::getEmptyKey() &&
         Offset != DenseMapInfo<int64_t>::getTombstoneKey();
}

std::optional<bool> checkedRangesOverlap(int64_t AStart, uint64_t ASize,
                                         int64_t BStart, uint64_t BSize) {
  if (ASize == 0 || BSize == 0)
    return false;
  if (ASize > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
      BSize > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
    return std::nullopt;
  std::optional<int64_t> AEnd =
      checkedOffsetAdd(AStart, static_cast<int64_t>(ASize));
  std::optional<int64_t> BEnd =
      checkedOffsetAdd(BStart, static_cast<int64_t>(BSize));
  if (!AEnd || !BEnd)
    return std::nullopt;
  return AStart < *BEnd && BStart < *AEnd;
}

// ===========================================================================
// Structural pointer chain walker
// ===========================================================================

// If V is `inttoptr(ptrtoint(x))` with matching pointer/int bit widths — a
// legal pointer-laundering round-trip — return x (the ptrtoint's pointer
// operand, after stripping offset-neutral `add/or X,0` chains the frontend
// may emit between the two casts). Tagged/masked encodings (any non-zero
// addend) are NOT round-trips and return nullptr.
//
// Shared by resolveVirtualRefImpl (identity resolution, case 7) and
// stripPointerCastsAndOffsets (offset resolution) so the two agree on exactly
// which laundered pointers are transparent: an `inttoptr` that is NOT a clean
// round-trip is opaque to both (identity -> nullopt, offset -> stop walking).
// Recognize an IntToPtr over both Instruction and ConstantExpr forms. LLVM
// has `PtrToIntOperator` but no `IntToPtrOperator`, so match via the generic
// Operator + opcode. Used at both call sites so identity and offset
// wrapper-stripping stay symmetric for the round-trip.
static bool isIntToPtrOp(const Value *V) {
  if (auto *Op = dyn_cast<Operator>(V))
    return Op->getOpcode() == Instruction::IntToPtr;
  return false;
}

static Value *getIntToPtrRoundTripInner(Value *V, const DataLayout &DL) {
  if (!isIntToPtrOp(V))
    return nullptr;
  auto *I2P = cast<Operator>(V);
  Value *Inner = I2P->getOperand(0);
  // Peel `add X, 0` / `or X, 0` chains between PtrToInt and IntToPtr; some
  // frontends emit these as part of constant-folded address arithmetic and
  // they are transparent (offset-neutral) to the round-trip. Use Operator +
  // opcode so ConstantExpr `add`/`or` is peeled too.
  for (unsigned StripDepth = 0; StripDepth < 8; ++StripDepth) {
    auto *BO = dyn_cast<Operator>(Inner);
    if (BO && (BO->getOpcode() == Instruction::Add ||
               BO->getOpcode() == Instruction::Or)) {
      if (auto *RHS = dyn_cast<ConstantInt>(BO->getOperand(1));
          RHS && RHS->isZero()) {
        Inner = BO->getOperand(0);
        continue;
      }
      if (auto *LHS = dyn_cast<ConstantInt>(BO->getOperand(0));
          LHS && LHS->isZero() && BO->getOpcode() == Instruction::Add) {
        Inner = BO->getOperand(1);
        continue;
      }
    }
    break;
  }
  // PtrToIntOperator covers both PtrToIntInst and ConstantExprPtrToInt.
  auto *P2I = dyn_cast<PtrToIntOperator>(Inner);
  if (!P2I)
    return nullptr;
  Type *PtrTy = P2I->getPointerOperand()->getType();
  if (auto *PT = dyn_cast<PointerType>(PtrTy)) {
    unsigned AS = PT->getAddressSpace();
    unsigned PtrBits = DL.getPointerSizeInBits(AS);
    unsigned IntBits = P2I->getType()->getIntegerBitWidth();
    if (PtrBits == IntBits)
      return P2I->getPointerOperand();
  }
  return nullptr;
}

Value *stripPointerCastsAndOffsets(Value *Ptr, const DataLayout &DL,
                                   int64_t *OutOffset, bool *Unresolved) {
  if (OutOffset)
    *OutOffset = 0;
  if (Unresolved)
    *Unresolved = false;
  if (!Ptr)
    return nullptr;

  int64_t Offset = 0;
  Value *V = Ptr;
  // Bound the walk defensively; Jeandle IR typically has < 5 layers.
  for (unsigned Depth = 0; Depth < 32; ++Depth) {
    if (auto *GEP = dyn_cast<GEPOperator>(V)) {
      const unsigned AS = GEP->getPointerAddressSpace();
      const unsigned IndexBits = DL.getIndexSizeInBits(AS);
      APInt Acc(IndexBits, 0, /*isSigned=*/true);
      if (!GEP->accumulateConstantOffset(DL, Acc)) {
        if (Unresolved)
          *Unresolved = true;
        return V;
      }
      std::optional<int64_t> GEPDelta = Acc.trySExtValue();
      std::optional<int64_t> NewOffset =
          GEPDelta ? checkedOffsetAdd(Offset, *GEPDelta) : std::nullopt;
      if (!NewOffset) {
        if (Unresolved)
          *Unresolved = true;
        return V;
      }
      Offset = *NewOffset;
      V = GEP->getPointerOperand();
      continue;
    }
    if (auto *BC = dyn_cast<BitCastOperator>(V)) {
      // Cross-address-space bitcasts are illegal IR; addrspacecast is required.
      // We don't assert here (this is a util used over arbitrary IR); just
      // continue on the inner operand.
      V = BC->getOperand(0);
      continue;
    }
    if (auto *ASC = dyn_cast<AddrSpaceCastOperator>(V)) {
      // Only chase through casts that stay in JavaHeapAddrSpace.
      auto *DstPT = dyn_cast<PointerType>(ASC->getType());
      auto *SrcPT = dyn_cast<PointerType>(ASC->getOperand(0)->getType());
      if (!DstPT || !SrcPT) {
        if (OutOffset)
          *OutOffset = Offset;
        return V;
      }
      if (DstPT->getAddressSpace() != jeandle::AddrSpace::JavaHeapAddrSpace ||
          SrcPT->getAddressSpace() != jeandle::AddrSpace::JavaHeapAddrSpace) {
        if (OutOffset)
          *OutOffset = Offset;
        return V;
      }
      V = ASC->getOperand(0);
      continue;
    }
    if (auto *FI = dyn_cast<FreezeInst>(V)) {
      V = FI->getOperand(0);
      continue;
    }
    // llvm.launder/strip.invariant.group and llvm.ptr.annotation are
    // pointer-identity-preserving.
    // resolveVirtualRef sees them through the alias map (processIntrinsic
    // installs it); offset resolution must peel them too, so a wrapped GEP
    // keeps its accumulated byte offset.
    if (auto *II = dyn_cast<IntrinsicInst>(V)) {
      Intrinsic::ID ID = II->getIntrinsicID();
      if (ID == Intrinsic::launder_invariant_group ||
          ID == Intrinsic::strip_invariant_group ||
          ID == Intrinsic::ptr_annotation) {
        V = II->getArgOperand(0);
        continue;
      }
    }
    // inttoptr(ptrtoint(x)) same-width round-trip (legal laundering): peel to
    // x so any GEP inside the round-trip contributes its offset. A non-round-
    // trip inttoptr is opaque — stop here. (resolveVirtualRef would not have
    // resolved such a pointer to a virtual, so the offset is unused there.)
    // getIntToPtrRoundTripInner uses Operator-form casts so a ConstantExpr
    // round-trip is peeled the same as an Instruction-form one — keeping this
    // offset path symmetric with the identity path.
    if (Value *Inner = getIntToPtrRoundTripInner(V, DL)) {
      V = Inner;
      continue;
    }
    break;
  }
  if (OutOffset)
    *OutOffset = Offset;
  return V;
}

// ===========================================================================
// resolveVirtualRef
// ===========================================================================

// Bound on structural recursion through PHI/Select. With Visited-set cycle
// detection the recursion is already bounded by the IR structure, but a hard
// depth cap prevents pathological diamond DAGs from causing exponential
// expansion through repeated PHI/Select operands.
static constexpr unsigned ResolveVirtualRefMaxDepth = 8;

static VirtualIdentityResult
resolveVirtualIdentityImpl(Value *V, const PEABlockState &State,
                           const AliasMap &Aliases, const DataLayout &DL,
                           VirtualIdentityMode Mode, DenseSet<Value *> &Visited,
                           unsigned Depth);

static VirtualIdentityResult checkAliasMap(Value *V, const PEABlockState &State,
                                           const AliasMap &Aliases) {
  // Virtual alias: a Value* registered as standing for some ObjectID.
  if (auto ID = Aliases.getVirtualAlias(V)) {
    if (const ObjectState *OS = State.getObjectStateOptional(*ID)) {
      if (OS->isVirtual())
        return VirtualIdentityResult::defined(*ID);
    }
    // Materialized or missing: V no longer denotes a virtual object.
    return VirtualIdentityResult::unknown();
  }
  // Scalar replacement aliases never resolve to a virtual object.
  if (Aliases.getScalarAlias(V) != nullptr)
    return VirtualIdentityResult::unknown();
  return VirtualIdentityResult::unknown();
}

// RAII helper: insert `V` into `Visited` on entry and erase on scope exit so
// that the set tracks "values currently on the DFS stack", not "values ever
// seen". The on-stack semantics correctly detect cycles (re-entering a value
// already being resolved) without rejecting legitimate diamond patterns where
// two sibling subtrees share a value (e.g. `select i1 %c, %o, %o`).
namespace {
struct StackGuard {
  DenseSet<Value *> &Set;
  Value *V;
  bool Inserted;
  StackGuard(DenseSet<Value *> &S, Value *Val) : Set(S), V(Val) {
    Inserted = Set.insert(V).second;
  }
  ~StackGuard() {
    if (Inserted)
      Set.erase(V);
  }
};
} // namespace

static VirtualIdentityResult
resolveVirtualIdentityImpl(Value *V, const PEABlockState &State,
                           const AliasMap &Aliases, const DataLayout &DL,
                           VirtualIdentityMode Mode, DenseSet<Value *> &Visited,
                           unsigned Depth) {
  if (!V)
    return VirtualIdentityResult::unknown();
  if (Depth > ResolveVirtualRefMaxDepth)
    return VirtualIdentityResult::unknown();
  // Cycle detection: if V is already on the resolution stack we're in a
  // self-reference (e.g. phi referencing itself) — bail.
  if (Visited.count(V))
    return VirtualIdentityResult::unknown();
  StackGuard Guard(Visited, V);

  // (1) Alias map lookup takes precedence over structural peeling so that
  // alias-registered Values (loads, PHIs, ...) resolve correctly even though
  // they have no structural relationship with their allocation site.
  if (Aliases.getVirtualAlias(V).has_value() ||
      Aliases.getScalarAlias(V) != nullptr) {
    if (Mode == VirtualIdentityMode::WholeObject) {
      std::optional<int64_t> Offset = resolveFieldOffset(V, DL);
      if (!Offset || *Offset != 0)
        return VirtualIdentityResult::unknown();
    }
    return checkAliasMap(V, State, Aliases);
  }

  // (2) Constants and special values.
  if (auto *C = dyn_cast<Constant>(V)) {
    if (isa<PoisonValue>(C))
      return VirtualIdentityResult::poisonWildcard();
    if (C->isNullValue() || isa<UndefValue>(C) || isa<GlobalValue>(C) ||
        isa<ConstantInt>(C) || isa<ConstantFP>(C) ||
        isa<ConstantPointerNull>(C))
      return VirtualIdentityResult::unknown();
    // ConstantExpr GEP/cast falls through to the structural cases below.
  }

  // (3) GEP — chase the base pointer (offset is resolveFieldOffset's job).
  if (auto *GEP = dyn_cast<GEPOperator>(V)) {
    if (Mode == VirtualIdentityMode::WholeObject) {
      std::optional<int64_t> Offset = resolveFieldOffset(V, DL);
      if (!Offset || *Offset != 0)
        return VirtualIdentityResult::unknown();
    }
    return resolveVirtualIdentityImpl(GEP->getPointerOperand(), State, Aliases,
                                      DL, Mode, Visited, Depth + 1);
  }

  // (4) AddrSpaceCast — only chase within JavaHeapAddrSpace.
  if (auto *ASC = dyn_cast<AddrSpaceCastOperator>(V)) {
    if (auto *DstPT = dyn_cast<PointerType>(ASC->getType()))
      if (DstPT->getAddressSpace() != jeandle::AddrSpace::JavaHeapAddrSpace)
        return VirtualIdentityResult::unknown();
    if (auto *SrcPT = dyn_cast<PointerType>(ASC->getOperand(0)->getType()))
      if (SrcPT->getAddressSpace() != jeandle::AddrSpace::JavaHeapAddrSpace)
        return VirtualIdentityResult::unknown();
    return resolveVirtualIdentityImpl(ASC->getOperand(0), State, Aliases, DL,
                                      Mode, Visited, Depth + 1);
  }

  // (5) BitCast.
  if (auto *BC = dyn_cast<BitCastOperator>(V))
    return resolveVirtualIdentityImpl(BC->getOperand(0), State, Aliases, DL,
                                      Mode, Visited, Depth + 1);

  // (6) Freeze preserves only an already-defined identity. Freezing poison,
  // undef, or an unresolved merge creates an arbitrary stable value, not a
  // virtual-object identity.
  if (auto *FI = dyn_cast<FreezeInst>(V)) {
    VirtualIdentityResult Inner = resolveVirtualIdentityImpl(
        FI->getOperand(0), State, Aliases, DL, Mode, Visited, Depth + 1);
    return Inner.isDefined() ? Inner : VirtualIdentityResult::unknown();
  }

  // (7) IntToPtr(PtrToInt(x)) round-trip with matching widths is a legal
  // laundering pattern (see getIntToPtrRoundTripInner); tagged-pointer
  // encodings (with masking/shifting) must escape. A non-round-trip inttoptr
  // is opaque — return nullopt so the caller materializes. isIntToPtrOp
  // covers both Instruction and ConstantExpr forms, keeping this symmetric
  // with the identity-resolution path above.
  if (isIntToPtrOp(V)) {
    if (Value *Inner = getIntToPtrRoundTripInner(V, DL))
      return resolveVirtualIdentityImpl(Inner, State, Aliases, DL, Mode,
                                        Visited, Depth + 1);
    return VirtualIdentityResult::unknown();
  }

  // (8) PHINode / (9) SelectInst — recursion through Case-B-style merges that
  // the AliasMap didn't pre-install (e.g. PHIs outside the current BlockState
  // domain, chained PHIs not yet processed, or Selects, which have no
  // Case-B-style pre-installation since Select isn't a PHI). If every operand
  // resolves to the same ObjectID, the merge denotes that virtual object on
  // every execution path. StackGuard guarantees that Visited reverts to the
  // caller's state on every return path, so a sibling subtree that shares a
  // value with an earlier sibling can still descend through it.
  if (auto *Phi = dyn_cast<PHINode>(V)) {
    std::optional<ObjectID> Common;
    for (Value *In : Phi->incoming_values()) {
      VirtualIdentityResult Sub = resolveVirtualIdentityImpl(
          In, State, Aliases, DL, Mode, Visited, Depth + 1);
      if (Sub.isPoisonWildcard())
        continue;
      if (!Sub.isDefined())
        return VirtualIdentityResult::unknown();
      if (!Common)
        Common = Sub.getObjectID();
      else if (Sub.getObjectID() != *Common)
        return VirtualIdentityResult::unknown();
    }
    if (Common)
      return VirtualIdentityResult::defined(*Common);
    // A merge made entirely from poison has no defined identity to refine.
    return VirtualIdentityResult::unknown();
  }

  if (auto *Sel = dyn_cast<SelectInst>(V)) {
    VirtualIdentityResult T = resolveVirtualIdentityImpl(
        Sel->getTrueValue(), State, Aliases, DL, Mode, Visited, Depth + 1);
    VirtualIdentityResult F = resolveVirtualIdentityImpl(
        Sel->getFalseValue(), State, Aliases, DL, Mode, Visited, Depth + 1);
    if (T.isPoisonWildcard() && F.isDefined())
      return F;
    if (F.isPoisonWildcard() && T.isDefined())
      return T;
    if (T.isDefined() && F.isDefined() && T.getObjectID() == F.getObjectID())
      return T;
    // In particular, all-poison selects are Unknown, as are any merges
    // involving undef/external values or differing defined identities.
    return VirtualIdentityResult::unknown();
  }

  // (10) Call, Argument, ... — opaque to alias analysis. Fall through.
  return VirtualIdentityResult::unknown();
}

VirtualIdentityResult resolveVirtualIdentity(Value *V,
                                             const PEABlockState &State,
                                             const AliasMap &Aliases,
                                             const DataLayout &DL,
                                             VirtualIdentityMode Mode) {
  DenseSet<Value *> Visited;
  return resolveVirtualIdentityImpl(V, State, Aliases, DL, Mode, Visited,
                                    /*Depth=*/0);
}

std::optional<ObjectID> resolveVirtualRef(Value *V, const PEABlockState &State,
                                          const AliasMap &Aliases,
                                          const DataLayout &DL) {
  VirtualIdentityResult R = resolveVirtualIdentity(
      V, State, Aliases, DL, VirtualIdentityMode::BaseObject);
  if (!R.isDefined())
    return std::nullopt;
  return R.getObjectID();
}

static bool isProvablyDistinctFromVirtualImpl(Value *V, ObjectID TargetID,
                                              const PEABlockState &State,
                                              const AliasMap &Aliases,
                                              const DataLayout &DL,
                                              DenseSet<Value *> &Visited,
                                              unsigned Depth) {
  if (!V || Depth > ResolveVirtualRefMaxDepth || Visited.count(V))
    return false;
  StackGuard Guard(Visited, V);

  if (isa<PoisonValue>(V) || isa<UndefValue>(V))
    return false;

  VirtualIdentityResult Whole = resolveVirtualIdentity(
      V, State, Aliases, DL, VirtualIdentityMode::WholeObject);
  if (Whole.isDefined())
    return Whole.getObjectID() != TargetID;

  // A materialized/stale alias no longer resolves through State, but the
  // AliasMap still records its allocation identity. Only its whole-object
  // address can participate in the allocation-site distinctness proof.
  if (auto AliasID = Aliases.getVirtualAlias(V)) {
    std::optional<int64_t> Offset = resolveFieldOffset(V, DL);
    return Offset && *Offset == 0 && *AliasID != TargetID;
  }
  if (Value *Scalar = Aliases.getScalarAlias(V))
    return isProvablyDistinctFromVirtualImpl(Scalar, TargetID, State, Aliases,
                                             DL, Visited, Depth + 1);

  if (auto *PN = dyn_cast<PHINode>(V)) {
    if (PN->getNumIncomingValues() == 0)
      return false;
    return llvm::all_of(PN->incoming_values(), [&](Value *Incoming) {
      return isProvablyDistinctFromVirtualImpl(Incoming, TargetID, State,
                                               Aliases, DL, Visited, Depth + 1);
    });
  }
  if (auto *Sel = dyn_cast<SelectInst>(V))
    return isProvablyDistinctFromVirtualImpl(Sel->getTrueValue(), TargetID,
                                             State, Aliases, DL, Visited,
                                             Depth + 1) &&
           isProvablyDistinctFromVirtualImpl(Sel->getFalseValue(), TargetID,
                                             State, Aliases, DL, Visited,
                                             Depth + 1);

  // Freeze makes an undef/poison choice observable. Recurse only when LLVM
  // can prove its operand already has a defined value.
  if (auto *FI = dyn_cast<FreezeInst>(V))
    return isGuaranteedNotToBeUndefOrPoison(FI->getOperand(0)) &&
           isProvablyDistinctFromVirtualImpl(FI->getOperand(0), TargetID, State,
                                             Aliases, DL, Visited, Depth + 1);

  if (auto *GEP = dyn_cast<GEPOperator>(V)) {
    std::optional<int64_t> Offset = resolveFieldOffset(V, DL);
    return Offset && *Offset == 0 &&
           isProvablyDistinctFromVirtualImpl(GEP->getPointerOperand(), TargetID,
                                             State, Aliases, DL, Visited,
                                             Depth + 1);
  }
  if (auto *BC = dyn_cast<BitCastOperator>(V))
    return isProvablyDistinctFromVirtualImpl(BC->getOperand(0), TargetID, State,
                                             Aliases, DL, Visited, Depth + 1);
  if (auto *ASC = dyn_cast<AddrSpaceCastOperator>(V)) {
    auto *SrcPT = dyn_cast<PointerType>(ASC->getOperand(0)->getType());
    auto *DstPT = dyn_cast<PointerType>(ASC->getType());
    if (!SrcPT || !DstPT ||
        SrcPT->getAddressSpace() != jeandle::AddrSpace::JavaHeapAddrSpace ||
        DstPT->getAddressSpace() != jeandle::AddrSpace::JavaHeapAddrSpace)
      return false;
    return isProvablyDistinctFromVirtualImpl(
        ASC->getOperand(0), TargetID, State, Aliases, DL, Visited, Depth + 1);
  }
  if (isIntToPtrOp(V)) {
    Value *Inner = getIntToPtrRoundTripInner(V, DL);
    return Inner &&
           isProvablyDistinctFromVirtualImpl(Inner, TargetID, State, Aliases,
                                             DL, Visited, Depth + 1);
  }

  if (auto *II = dyn_cast<IntrinsicInst>(V)) {
    switch (II->getIntrinsicID()) {
    case Intrinsic::launder_invariant_group:
    case Intrinsic::strip_invariant_group:
    case Intrinsic::ptr_annotation:
      return isProvablyDistinctFromVirtualImpl(II->getArgOperand(0), TargetID,
                                               State, Aliases, DL, Visited,
                                               Depth + 1);
    default:
      return false;
    }
  }

  if (isa<ConstantPointerNull>(V) || isa<GlobalValue>(V))
    return true;
  if (isa<Constant>(V))
    return false;

  // A still-virtual allocation has not been published. A direct external SSA
  // reference cannot denote it. Unknown pointer-producing structure is not
  // accepted: it may merely be another carrier for TargetID.
  return isa<Argument>(V) || isa<LoadInst>(V) || isa<CallBase>(V);
}

bool isProvablyDistinctFromVirtual(Value *V, ObjectID TargetID,
                                   const PEABlockState &State,
                                   const AliasMap &Aliases,
                                   const DataLayout &DL) {
  assert(TargetID != InvalidObjectID);
  DenseSet<Value *> Visited;
  return isProvablyDistinctFromVirtualImpl(V, TargetID, State, Aliases, DL,
                                           Visited, /*Depth=*/0);
}

// ===========================================================================
// resolveFieldOffset
// ===========================================================================

std::optional<int64_t> resolveFieldOffset(Value *Ptr, const DataLayout &DL) {
  if (!Ptr)
    return std::nullopt;
  // Delegate to the shared offset accumulator so identity- and
  // offset-resolution peel the same set of wrappers (GEP constant offsets,
  // bitcast, JavaHeap addrspacecast, freeze, launder/strip.invariant.group,
  // ptr.annotation, inttoptr(ptrtoint(x)) round-trip): a pointer that resolves
  // to a virtual base yields its true byte offset here. A non-constant GEP ->
  // nullopt (caller materializes); a non-GEP base (the object itself, a
  // whole-object Case-B PHI/Select) -> 0.
  int64_t Off = 0;
  bool NonConst = false;
  stripPointerCastsAndOffsets(Ptr, DL, &Off, &NonConst);
  if (NonConst || !isUsableFieldOffset(Off))
    return std::nullopt;
  return Off;
}

bool hasUnremovedSemanticUses(Value *Root,
                              function_ref<bool(const Use &)> IsRemoved) {
  SmallPtrSet<Value *, 16> Visited;
  SmallVector<Value *, 16> Worklist(1, Root);
  while (!Worklist.empty()) {
    Value *Current = Worklist.pop_back_val();
    if (!Current || !Visited.insert(Current).second)
      continue;
    for (const Use &U : Current->uses()) {
      auto *I = dyn_cast<Instruction>(U.getUser());
      bool IsCarrier = I && (isa<GetElementPtrInst>(I) || isa<BitCastInst>(I) ||
                             isa<AddrSpaceCastInst>(I) || isa<FreezeInst>(I) ||
                             isa<PHINode>(I) || isa<SelectInst>(I));
      if (auto *II = dyn_cast_or_null<IntrinsicInst>(I)) {
        Intrinsic::ID ID = II->getIntrinsicID();
        IsCarrier = U.getOperandNo() == 0 &&
                    (ID == Intrinsic::launder_invariant_group ||
                     ID == Intrinsic::strip_invariant_group ||
                     ID == Intrinsic::ptr_annotation);
      }
      if (IsCarrier) {
        Worklist.push_back(I);
        continue;
      }
      if (!IsRemoved(U))
        return true;
    }
  }
  return false;
}

// ===========================================================================
// Misc
// ===========================================================================

uintptr_t extractAllocationKlass(const CallBase *AllocCB) {
  if (!AllocCB || AllocCB->arg_size() == 0)
    return 0;
  // Both jeandle.new_instance and jeandle.new_array take the klass pointer as
  // their first operand.
  return jeandle::extractKlassConstant(
      const_cast<Value *>(AllocCB->getArgOperand(0)));
}

std::optional<uint32_t> extractInstanceSize(const CallBase *NewInstance) {
  if (!isJeandleNewInstance(NewInstance) || NewInstance->arg_size() < 2)
    return std::nullopt;
  auto *CI = dyn_cast<ConstantInt>(NewInstance->getArgOperand(1));
  if (!CI)
    return std::nullopt;
  return static_cast<uint32_t>(CI->getZExtValue());
}

std::optional<uint32_t> extractArrayLength(const CallBase *NewArray) {
  if (!isJeandleNewArray(NewArray) || NewArray->arg_size() != 5)
    return std::nullopt;
  auto *CI = dyn_cast<ConstantInt>(NewArray->getArgOperand(1));
  if (!CI)
    return std::nullopt;
  return static_cast<uint32_t>(CI->getZExtValue());
}

// ===========================================================================
// Deopt bundle scope structure
// ===========================================================================

std::optional<unsigned>
findInnermostDeoptScopeBCIPairStart(const CallBase &CB) {
  auto Deopt = CB.getOperandBundle(LLVMContext::OB_deopt);
  if (!Deopt)
    return std::nullopt;
  for (unsigned I = Deopt->Inputs.size(); I > 1; --I) {
    auto *BCI0 = dyn_cast<ConstantInt>(Deopt->Inputs[I - 2].get());
    auto *BCI1 = dyn_cast<ConstantInt>(Deopt->Inputs[I - 1].get());
    if (!BCI0 || !BCI1 || !BCI0->getType()->isIntegerTy(32) ||
        !BCI1->getType()->isIntegerTy(32))
      continue;
    if (BCI0->getSExtValue() != BCI1->getSExtValue())
      return std::nullopt; // malformed pair — scope boundaries unknown.
    return I - 2;
  }
  return std::nullopt;
}

std::optional<unsigned> findFirstDeoptScopeBCIPairStart(const CallBase &CB) {
  auto Deopt = CB.getOperandBundle(LLVMContext::OB_deopt);
  if (!Deopt)
    return std::nullopt;
  for (unsigned I = 2; I <= Deopt->Inputs.size(); ++I) {
    auto *BCI0 = dyn_cast<ConstantInt>(Deopt->Inputs[I - 2].get());
    auto *BCI1 = dyn_cast<ConstantInt>(Deopt->Inputs[I - 1].get());
    if (!BCI0 || !BCI1 || !BCI0->getType()->isIntegerTy(32) ||
        !BCI1->getType()->isIntegerTy(32))
      continue;
    if (BCI0->getSExtValue() != BCI1->getSExtValue())
      return std::nullopt; // malformed pair — scope boundaries unknown.
    return I - 2;
  }
  return std::nullopt;
}

} // namespace llvm::jeandle::pea
