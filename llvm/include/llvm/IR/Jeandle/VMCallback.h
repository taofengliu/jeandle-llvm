//===- VMCallback.h - VM Callback Interface -------------------------------===//
//
// Copyright (c) 2025, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Callback interface from LLVM optimization passes to the JVM.
// The JVM registers callbacks during compiler initialization; LLVM passes
// invoke them to query VM-level type hierarchy information.
//
//===----------------------------------------------------------------------===//

#ifndef JEANDLE_VM_CALLBACK_H
#define JEANDLE_VM_CALLBACK_H

#include <cstdint>

namespace llvm::jeandle {

// =============================================================================
// Callback type descriptors (used by logging/replay schema)
// =============================================================================

/// Value type for VM callback parameters and results.
enum class VMCallbackValueType : uint8_t {
  Bool,    // bool
  Uintptr, // uintptr_t
  Int,     // int
};

// =============================================================================
// Master callback list — add new callbacks here
// =============================================================================
//
// ALL_JEANDLE_VM_CALLBACKS(def) invokes `def` for each VM callback with:
//   Name     — callback name (struct field, CK_ prefix, stringification)
//   RetType  — C++ return type (bool, uintptr_t)
//   ResType  — VMCallbackValueType enum suffix (Bool, Uintptr, Int)
//   Params   — parenthesized parameter declarations,
//              e.g. (uintptr_t a1, uintptr_t a2)
//   Args     — parenthesized argument names, e.g. (a1, a2)
//   ArgTypes — parenthesized VMCallbackValueType enum values, e.g.
//              (VMCallbackValueType::Uintptr, VMCallbackValueType::Uintptr)
//   NumArgs  — number of arguments
//
// -----------------------------------------------------------------------------
// CRITICAL: Sequential Consistency Requirement
//
// The VMCallback Replay mechanism relies on a strict total ordering of callback
// invocations. For a replay to be successful, the sequence of VM calls during
// compilation must be identical to the sequence recorded in the log.
//
// Developers must ensure that any logic triggering VM callbacks follows a
// deterministic execution path. Avoid issuing callbacks while iterating over
// unordered containers (e.g., llvm::DenseMap or std::unordered_map).
//
// Correct Example:
//   for (Instruction &I : instructions(F)) { triggerCallback(I); }
//
// Incorrect Example:
//   for (auto &Entry : UnorderedMap) { triggerCallback(Entry.second); }
// -----------------------------------------------------------------------------
//
// To add a new callback, add one row below, then implement the JDK-side
// function in jeandleVMCallback.cpp and wire it in
// register_jeandle_vm_callbacks().
//
#define ALL_JEANDLE_VM_CALLBACKS(def)                                            \
  def(IsSubtype, bool, Bool,                                                     \
      (uintptr_t a1, uintptr_t a2), (a1, a2),                                    \
      (VMCallbackValueType::Uintptr, VMCallbackValueType::Uintptr), 2)           \
  def(GetCommonSuperKlass, uintptr_t, Uintptr,                                   \
      (uintptr_t a1, uintptr_t a2), (a1, a2),                                    \
      (VMCallbackValueType::Uintptr, VMCallbackValueType::Uintptr), 2)           \
  def(GetFieldType, uintptr_t, Uintptr,                                          \
      (uintptr_t a1, int a2), (a1, a2),                                          \
      (VMCallbackValueType::Uintptr, VMCallbackValueType::Int), 2)               \
  def(IsInterface, bool, Bool,                                                   \
      (uintptr_t a1), (a1),                                                      \
      (VMCallbackValueType::Uintptr), 1)                                         \
  def(IsObjectKlass, bool, Bool,                                                 \
      (uintptr_t a1), (a1),                                                      \
      (VMCallbackValueType::Uintptr), 1)                                         \
  def(IsEffectivelyFinal, bool, Bool,                                            \
      (uintptr_t a1), (a1),                                                      \
      (VMCallbackValueType::Uintptr), 1)

// =============================================================================
// VMCallbacks struct — generated from master list
// =============================================================================

/// Generate a typedef + struct field for each callback.
/// Params retains its outer parentheses, which become the function-pointer
/// parameter list: RetType (*) Params => e.g. bool (*)(uintptr_t a1, uintptr_t
/// a2)
#define DEF_VM_CALLBACK_FIELD(Name, RetType, ResType, Params, Args, ArgTypes,  \
                              NumArgs)                                         \
  using Name##Fn = RetType(*) Params;                                          \
  Name##Fn Name = nullptr;

/// All VM callbacks used by Jeandle LLVM passes.
///
/// Fields (generated from ALL_JEANDLE_VM_CALLBACKS above):
///   IsSubtype           — Returns true if SubKlass is a subtype of SuperKlass.
///   GetCommonSuperKlass — Given two klass pointers, return their lowest common
///                         ancestor. Returns 0 if unknown.
///   GetFieldType        — Given a klass pointer and byte offset, return the
///                         field's declared type klass pointer. Returns 0 if
///                         unknown.
///   IsInterface         — Returns true if the klass is an interface.
///   IsObjectKlass       — Returns true if the klass is java.lang.Object.
///   IsEffectivelyFinal  — Returns true if no subclass can exist at runtime.
struct VMCallbacks {
  ALL_JEANDLE_VM_CALLBACKS(DEF_VM_CALLBACK_FIELD)
};

#undef DEF_VM_CALLBACK_FIELD

/// Register VM callbacks. Must be called before running the optimization
/// pipeline (typically once during compiler initialization).
void registerVMCallbacks(const VMCallbacks &CB);

/// Retrieve the registered VM callbacks. Returns nullptr if not registered.
const VMCallbacks *getVMCallbacks();

} // namespace llvm::jeandle

#endif // JEANDLE_VM_CALLBACK_H
