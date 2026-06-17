//===- PartialEscapeUtils.h - PEA helpers ------------------------*- C++ -*-===//
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
bool isJeandleAllocation(const CallBase *CB);     // either of the above
bool isJeandleArrayLength(const CallBase *CB);
bool isJeandleLoadKlass(const CallBase *CB);
bool isJeandleCheckCast(const CallBase *CB);
bool isJeandleInstanceOf(const CallBase *CB);
bool isJeandleCheckInstanceOf(const CallBase *CB);
bool isJeandleCheckIfValueBased(const CallBase *CB);
bool isJeandleArrayStoreCheck(const CallBase *CB);
bool isJeandleMonitorEnter(const CallBase *CB);
bool isJeandleMonitorExit(const CallBase *CB);
bool isJeandleRegisterFinalizerIfNeeded(const CallBase *CB);

// Extract the Java element basic type from an array klass pointer that we
// already have in hand (uintptr_t).  Currently a stub that returns
// std::nullopt — a future hook into VMCallbacks will provide this.
std::optional<JBasicType> elementTypeForArrayKlass(uintptr_t ArrayKlass);

// Map a JBasicType to its LLVM IR storage type for one element (e.g., Byte→i8,
// Object→ptr addrspace(1)).  Returns nullptr if Kind == Count.
Type *llvmElementTypeFor(JBasicType Kind, LLVMContext &Ctx);

// Strip pointer-identity-preserving operations (bitcast, addrspacecast within
// addrspace(1), freeze) and constant-offset GEPs, accumulating the constant
// offset into *OutOffset.  Returns the root pointer.  Sets *NonConstant = true
// if a non-constant GEP index was encountered (in which case the accumulated
// offset is invalid).
//
// This is the structural helper that both resolveVirtualRef and
// resolveFieldOffset use.
Value *stripPointerCastsAndOffsets(Value *Ptr, const DataLayout &DL,
                                   int64_t *OutOffset, bool *NonConstant);

// Resolve V to the ObjectID of the virtual object it names, in the current
// State.  Returns std::nullopt if V does not resolve to a virtual object.
// Algorithm cycle-safe via a small visited set.
//
// This is the canonical implementation; PEABlockState::resolveVirtualRef
// delegates here.
std::optional<ObjectID> resolveVirtualRef(Value *V,
                                          const PEABlockState &State,
                                          const AliasMap &Aliases,
                                          const DataLayout &DL);

// Resolve V to the byte offset from its (presumed virtual) base pointer.
// Returns std::nullopt if V is not a constant-offset access (e.g., GEP with
// non-constant index, inttoptr, or unrecognised pattern).
std::optional<int64_t> resolveFieldOffset(Value *Ptr, const DataLayout &DL);

// True iff V is a pointer in the JavaHeapAddrSpace (ptr addrspace(1)).
bool isJavaHeapPointer(const Value *V);

// Returns the klass pointer (as uintptr_t) attached to the allocation call's
// klass argument, or 0 if not statically known.  Wraps
// jeandle::extractKlassConstant for the allocation-specific operand index.
uintptr_t extractAllocationKlass(const CallBase *AllocCB);

// Returns the constant size-in-bytes operand of jeandle.new_instance.
// std::nullopt if non-constant.
std::optional<uint32_t> extractInstanceSize(const CallBase *NewInstance);

// Returns the constant length operand of jeandle.newarray.
// std::nullopt if non-constant.
std::optional<uint32_t> extractArrayLength(const CallBase *NewArray);

} // namespace llvm::jeandle::pea

#endif
