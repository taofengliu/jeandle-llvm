//===-- PartialEscapeUtils.cpp - PEA helper implementations ---*- C++ -*-===//
//
// Part of the Jeandle JIT compiler.
//
// Pure helpers used by both the analysis and the transform pass.  No state.
// See PartialEscapeUtils.h for the interface and PEA-Plan sections 2.3, 2.7,
// and 2.8 for the underlying design.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Jeandle/PartialEscapeUtils.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Jeandle/JavaType.h"
#include "llvm/IR/Jeandle/Metadata.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Type.h"

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
  return isJeandleCallNamed(CB, "jeandle.newarray");
}

bool isJeandleAllocation(const CallBase *CB) {
  return isJeandleNewInstance(CB) || isJeandleNewArray(CB);
}

bool isJeandleArrayLength(const CallBase *CB) {
  return isJeandleCallNamed(CB, "jeandle.array_length");
}

bool isJeandleLoadKlass(const CallBase *CB) {
  return isJeandleCallNamed(CB, "jeandle.load_klass");
}

bool isJeandleCheckCast(const CallBase *CB) {
  return isJeandleCallNamed(CB, "jeandle.check_cast");
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

// ===========================================================================
// Type / klass helpers
// ===========================================================================

std::optional<JBasicType> elementTypeForArrayKlass(uintptr_t ArrayKlass) {
  // TODO: wire to VMCallbacks::ElementBasicTypeOf(ArrayKlass).
  // Until that VM callback exists, return nullopt so that callers degrade
  // gracefully (the allocation will be treated as non-virtualizable).
  (void)ArrayKlass;
  return std::nullopt;
}

Type *llvmElementTypeFor(JBasicType Kind, LLVMContext &Ctx) {
  switch (Kind) {
  case JBasicType::Boolean: return Type::getInt1Ty(Ctx);
  case JBasicType::Byte:    return Type::getInt8Ty(Ctx);
  case JBasicType::Char:    return Type::getInt16Ty(Ctx);
  case JBasicType::Short:   return Type::getInt16Ty(Ctx);
  case JBasicType::Int:     return Type::getInt32Ty(Ctx);
  case JBasicType::Long:    return Type::getInt64Ty(Ctx);
  case JBasicType::Float:   return Type::getFloatTy(Ctx);
  case JBasicType::Double:  return Type::getDoubleTy(Ctx);
  case JBasicType::Object:
    return PointerType::get(Ctx, jeandle::AddrSpace::JavaHeapAddrSpace);
  case JBasicType::Count:
    return nullptr;
  }
  return nullptr;
}

// ===========================================================================
// Structural pointer chain walker
// ===========================================================================

Value *stripPointerCastsAndOffsets(Value *Ptr, const DataLayout &DL,
                                   int64_t *OutOffset, bool *NonConstant) {
  if (OutOffset)
    *OutOffset = 0;
  if (NonConstant)
    *NonConstant = false;
  if (!Ptr)
    return nullptr;

  Value *V = Ptr;
  // Bound the walk defensively; Jeandle IR typically has < 5 layers.
  for (unsigned Depth = 0; Depth < 32; ++Depth) {
    if (auto *GEP = dyn_cast<GEPOperator>(V)) {
      const unsigned AS = GEP->getPointerAddressSpace();
      const unsigned PtrBits = DL.getPointerSizeInBits(AS);
      APInt Acc(PtrBits, 0, /*isSigned=*/true);
      if (!GEP->accumulateConstantOffset(DL, Acc)) {
        if (NonConstant)
          *NonConstant = true;
        return V;
      }
      if (OutOffset)
        *OutOffset += Acc.getSExtValue();
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
      if (!DstPT || !SrcPT)
        return V;
      if (DstPT->getAddressSpace() != jeandle::AddrSpace::JavaHeapAddrSpace ||
          SrcPT->getAddressSpace() != jeandle::AddrSpace::JavaHeapAddrSpace)
        return V;
      V = ASC->getOperand(0);
      continue;
    }
    if (auto *FI = dyn_cast<FreezeInst>(V)) {
      V = FI->getOperand(0);
      continue;
    }
    break;
  }
  return V;
}

// ===========================================================================
// resolveVirtualRef
// ===========================================================================

static std::optional<ObjectID>
resolveVirtualRefImpl(Value *V, const PEABlockState &State,
                      const AliasMap &Aliases, const DataLayout &DL,
                      DenseSet<Value *> &Visited);

static std::optional<ObjectID>
checkAliasMap(Value *V, const PEABlockState &State, const AliasMap &Aliases) {
  // Virtual alias: a Value* registered as standing for some ObjectID.
  if (auto ID = Aliases.getVirtualAlias(V)) {
    if (const ObjectState *OS = State.getObjectStateOptional(*ID)) {
      if (OS->isVirtual())
        return *ID;
    }
    // Materialized or missing: V no longer denotes a virtual object.
    return std::nullopt;
  }
  // Scalar replacement aliases never resolve to a virtual object.
  if (Aliases.getScalarAlias(V) != nullptr)
    return std::nullopt;
  return std::nullopt;
}

static std::optional<ObjectID>
resolveVirtualRefImpl(Value *V, const PEABlockState &State,
                      const AliasMap &Aliases, const DataLayout &DL,
                      DenseSet<Value *> &Visited) {
  if (!V)
    return std::nullopt;
  if (!Visited.insert(V).second)
    return std::nullopt;

  // (1) Alias map lookup takes precedence over structural peeling so that
  // alias-registered Values (loads, PHIs, ...) resolve correctly even though
  // they have no structural relationship with their allocation site.
  if (Aliases.getVirtualAlias(V).has_value() ||
      Aliases.getScalarAlias(V) != nullptr)
    return checkAliasMap(V, State, Aliases);

  // (2) Constants and special values.
  if (auto *C = dyn_cast<Constant>(V)) {
    if (C->isNullValue() || isa<UndefValue>(C) || isa<PoisonValue>(C) ||
        isa<GlobalValue>(C) || isa<ConstantInt>(C) || isa<ConstantFP>(C) ||
        isa<ConstantPointerNull>(C))
      return std::nullopt;
    // ConstantExpr GEP/cast falls through to the structural cases below.
  }

  // (3) GEP — chase the base pointer (offset is resolveFieldOffset's job).
  if (auto *GEP = dyn_cast<GEPOperator>(V))
    return resolveVirtualRefImpl(GEP->getPointerOperand(), State, Aliases, DL,
                                 Visited);

  // (4) AddrSpaceCast — only chase within JavaHeapAddrSpace.
  if (auto *ASC = dyn_cast<AddrSpaceCastOperator>(V)) {
    if (auto *DstPT = dyn_cast<PointerType>(ASC->getType()))
      if (DstPT->getAddressSpace() != jeandle::AddrSpace::JavaHeapAddrSpace)
        return std::nullopt;
    if (auto *SrcPT = dyn_cast<PointerType>(ASC->getOperand(0)->getType()))
      if (SrcPT->getAddressSpace() != jeandle::AddrSpace::JavaHeapAddrSpace)
        return std::nullopt;
    return resolveVirtualRefImpl(ASC->getOperand(0), State, Aliases, DL,
                                 Visited);
  }

  // (5) BitCast.
  if (auto *BC = dyn_cast<BitCastOperator>(V))
    return resolveVirtualRefImpl(BC->getOperand(0), State, Aliases, DL,
                                 Visited);

  // (6) Freeze.
  if (auto *FI = dyn_cast<FreezeInst>(V))
    return resolveVirtualRefImpl(FI->getOperand(0), State, Aliases, DL,
                                 Visited);

  // (7) IntToPtr(PtrToInt(x)) round-trip with matching widths is a legal
  // laundering pattern; tagged-pointer encodings (with masking/shifting) must
  // escape and are caught by the fall-through.
  if (auto *I2P = dyn_cast<IntToPtrInst>(V)) {
    if (auto *P2I = dyn_cast<PtrToIntInst>(I2P->getOperand(0))) {
      Type *PtrTy = P2I->getPointerOperand()->getType();
      if (auto *PT = dyn_cast<PointerType>(PtrTy)) {
        unsigned AS = PT->getAddressSpace();
        unsigned PtrBits = DL.getPointerSizeInBits(AS);
        unsigned IntBits = P2I->getType()->getIntegerBitWidth();
        if (PtrBits == IntBits)
          return resolveVirtualRefImpl(P2I->getPointerOperand(), State, Aliases,
                                       DL, Visited);
      }
    }
    return std::nullopt;
  }

  // (8) PHI, Select, Call, Argument, ... — no structural recursion.
  return std::nullopt;
}

std::optional<ObjectID> resolveVirtualRef(Value *V,
                                          const PEABlockState &State,
                                          const AliasMap &Aliases,
                                          const DataLayout &DL) {
  DenseSet<Value *> Visited;
  return resolveVirtualRefImpl(V, State, Aliases, DL, Visited);
}

// ===========================================================================
// resolveFieldOffset
// ===========================================================================

std::optional<int64_t> resolveFieldOffset(Value *Ptr, const DataLayout &DL) {
  if (!Ptr)
    return std::nullopt;

  if (auto *GEP = dyn_cast<GEPOperator>(Ptr)) {
    const unsigned PtrBits =
        DL.getPointerSizeInBits(GEP->getPointerAddressSpace());

    // Pattern 1: Jeandle-canonical i8-typed single-index GEP.
    if (GEP->getNumIndices() == 1 &&
        GEP->getSourceElementType()->isIntegerTy(8)) {
      if (auto *CI = dyn_cast<ConstantInt>(GEP->getOperand(1))) {
        APInt V = CI->getValue().sextOrTrunc(PtrBits);
        return V.getSExtValue();
      }
      return std::nullopt;
    }

    // Patterns 2/3: arbitrary GEP with all-constant indices.
    APInt Acc(PtrBits, 0, /*isSigned=*/true);
    if (GEP->accumulateConstantOffset(DL, Acc))
      return Acc.getSExtValue();

    return std::nullopt;
  }

  // Pattern 3.5: freeze on a pointer is a pointer-identity-preserving
  // passthrough; peel and retry against the operand. Mirrors the FreezeInst
  // handling in stripPointerCastsAndOffsets and resolveVirtualRefImpl.
  if (auto *FI = dyn_cast<FreezeInst>(Ptr))
    return resolveFieldOffset(FI->getOperand(0), DL);

  // Pattern 4: cast / alias chain to a GEP — strip and retry.  Guard against
  // infinite recursion: only recurse if stripping actually changes the value.
  Value *Stripped = Ptr->stripPointerCastsAndAliases();
  if (Stripped != Ptr)
    return resolveFieldOffset(Stripped, DL);

  // Pattern 5: no GEP — the access targets the base of the (presumed virtual)
  // object.  Offset is zero by construction of the IR.  The caller is
  // responsible for first establishing via resolveVirtualRef that Ptr does
  // resolve to a virtual base; this function only reports the offset.
  return 0;
}

// ===========================================================================
// Misc
// ===========================================================================

bool isJavaHeapPointer(const Value *V) {
  if (!V)
    return false;
  Type *Ty = V->getType();
  if (!Ty || !Ty->isPointerTy())
    return false;
  return cast<PointerType>(Ty)->getAddressSpace() ==
         jeandle::AddrSpace::JavaHeapAddrSpace;
}

uintptr_t extractAllocationKlass(const CallBase *AllocCB) {
  if (!AllocCB || AllocCB->arg_size() == 0)
    return 0;
  // Both jeandle.new_instance and jeandle.newarray take the klass pointer as
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
  if (!isJeandleNewArray(NewArray) || NewArray->arg_size() < 2)
    return std::nullopt;
  auto *CI = dyn_cast<ConstantInt>(NewArray->getArgOperand(1));
  if (!CI)
    return std::nullopt;
  return static_cast<uint32_t>(CI->getZExtValue());
}

} // namespace llvm::jeandle::pea
