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
      (VMCallbackValueType::Uintptr), 1)                                         \
  def(RequiresStrictLockOrder, int, Int,                                         \
      (), (),                                                                    \
      (), 0)                                                                     \
  def(ElementBasicTypeOfArrayKlass, int, Int,                                    \
      (uintptr_t a1), (a1),                                                      \
      (VMCallbackValueType::Uintptr), 1)                                         \
  def(ArrayElementKlass, uintptr_t, Uintptr,                                     \
      (uintptr_t a1), (a1),                                                      \
      (VMCallbackValueType::Uintptr), 1)                                         \
  def(IsValueBased, bool, Bool,                                                  \
      (uintptr_t a1), (a1),                                                      \
      (VMCallbackValueType::Uintptr), 1)                                         \
  def(IsBoxed, int, Int,                                                         \
      (uintptr_t a1), (a1),                                                      \
      (VMCallbackValueType::Uintptr), 1)                                         \
  def(HasFinalizer, bool, Bool,                                                  \
      (uintptr_t a1), (a1),                                                      \
      (VMCallbackValueType::Uintptr), 1)                                         \
  def(CanVirtualize, bool, Bool,                                                 \
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
///   RequiresStrictLockOrder — Returns 1 if the runtime requires strict
///                         monitor-stack nesting at materialization
///                         (HotSpot LM_LIGHTWEIGHT); 0 otherwise.
///   ElementBasicTypeOfArrayKlass — Given an array klass pointer, return the
///                         Java basic type of its elements encoded as a
///                         JBasicType integer (Boolean=0..Object=8). Returns
///                         JBasicType::Count (9) for non-array klasses or
///                         null/unknown inputs.
///   ArrayElementKlass    — Given an object-array klass pointer, return its
///                         element klass pointer. Returns 0 for primitive
///                         (type-array) klasses, for null inputs, or when
///                         the element klass is otherwise unavailable.
///   IsValueBased         — Returns true iff the klass carries the
///                         jdk.internal.ValueBased annotation (HotSpot's
///                         access_flags().is_value_based_class()). Used by
///                         PEA's foldCheckIfValueBased to decide whether a
///                         virtual receiver of jeandle.check_if_value_based
///                         should force-materialize (klass IS value-based —
///                         the runtime warning must fire on a real oop) or
///                         elide the check (klass is NOT value-based). For
///                         null/unresolved klass inputs the implementation
///                         must return false (PEA already gates the query
///                         on VObj.Klass != 0, so any false is treated
///                         conservatively).
///   IsBoxed              — Returns the JBasicType integer of the boxed
///                         primitive if the klass is one of the eight
///                         autobox wrapper classes (java.lang.Boolean,
///                         Byte, Character, Short, Integer, Long, Float,
///                         Double); returns JBasicType::Count (9) for any
///                         other klass or null input. Used by partial
///                         escape analysis to mark a virtual Instance VO
///                         as a "boxed primitive" so the icmp eq fold can
///                         perform a structural value comparison
///                         (mirroring Graal's VirtualBoxingNode +
///                         ObjectEqualsNode boxed path), and so
///                         materialization can route through the
///                         box-cache valueOf entry point rather than a
///                         fresh allocation.
///   HasFinalizer         — Returns true iff the klass has a non-trivial
///                         finalize() method (HotSpot's
///                         InstanceKlass::has_finalizer()). Mirrors
///                         Graal's NewInstanceNode + RegisterFinalizerNode
///                         interaction: a class with a finalizer must
///                         have its allocation actually executed at the
///                         original allocation site so HotSpot's
///                         finalizer registration runs, otherwise the
///                         finalize() method would never be invoked.
///                         tier1Allocate refuses to virtualize the alloc
///                         when this returns true. For null inputs the
///                         implementation must return false.
///   CanVirtualize        — Returns true iff the klass is safe to
///                         virtualize (no identity-sensitive runtime
///                         constraints). Mirrors Graal's
///                         MetaAccessExtensionProvider.canVirtualize.
///                         The JDK-side implementation enumerates
///                         identity-sensitive subtypes (java.lang.ref.
///                         Reference and Thread hierarchies) and returns
///                         false for those; everything else returns
///                         true. tier1Allocate refuses to virtualize
///                         when this returns false. For null inputs the
///                         implementation must return false (analogous
///                         to the value-based / boxed callbacks: a null
///                         klass cannot be proven safe).
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
