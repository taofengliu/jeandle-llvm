//===-- VMConstants.cpp - Jeandle VM layout constants impl -----*- C++ -*-===//
//
// Part of the Jeandle JIT compiler.
//
// Implementation of the global VMConstants singleton and the static
// classifyType helper.  See VMConstants.h for the rationale.
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/Jeandle/VMConstants.h"

#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Jeandle/Metadata.h"
#include "llvm/IR/Type.h"

namespace llvm {
namespace jeandle {

static VMConstants GlobalVMConstants;

const VMConstants &getVMConstants() { return GlobalVMConstants; }
void setVMConstants(VMConstants C) { GlobalVMConstants = std::move(C); }

JBasicType VMConstants::classifyType(Type *Ty) {
  if (!Ty)
    return JBasicType::Count;
  if (Ty->isPointerTy()) {
    auto *PT = cast<PointerType>(Ty);
    if (PT->getAddressSpace() == jeandle::AddrSpace::JavaHeapAddrSpace)
      return JBasicType::Object;
    return JBasicType::Count;
  }
  if (Ty->isIntegerTy()) {
    switch (Ty->getIntegerBitWidth()) {
    case 1:  return JBasicType::Boolean;
    case 8:  return JBasicType::Byte;
    case 16: return JBasicType::Short;
    case 32: return JBasicType::Int;
    case 64: return JBasicType::Long;
    default: return JBasicType::Count;
    }
  }
  if (Ty->isFloatTy())
    return JBasicType::Float;
  if (Ty->isDoubleTy())
    return JBasicType::Double;
  return JBasicType::Count;
}

} // namespace jeandle
} // namespace llvm
