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

/// All VM callbacks used by Jeandle LLVM passes.
struct VMCallbacks {
  /// Returns true if SubKlass is a subtype of SuperKlass.
  using IsSubtypeFn = bool (*)(uintptr_t SubKlass, uintptr_t SuperKlass);
  IsSubtypeFn IsSubtype = nullptr;

  /// Given two klass pointers, return their lowest common ancestor (LCA).
  /// Returns 0 if the common supertype cannot be determined.
  using CommonSuperKlassFn = uintptr_t (*)(uintptr_t KlassPtr1,
                                           uintptr_t KlassPtr2);
  CommonSuperKlassFn GetCommonSuperKlass = nullptr;

  /// Given a klass pointer and byte offset, return the field's declared type
  /// klass pointer. Returns 0 if unknown or non-object field.
  using FieldTypeFn = uintptr_t (*)(uintptr_t KlassPtr, int Offset);
  FieldTypeFn GetFieldType = nullptr;

  /// Returns true if the klass is an interface.
  using IsInterfaceFn = bool (*)(uintptr_t KlassPtr);
  IsInterfaceFn IsInterface = nullptr;

  /// Returns true if the klass is java.lang.Object.
  using IsObjectKlassFn = bool (*)(uintptr_t KlassPtr);
  IsObjectKlassFn IsObjectKlass = nullptr;

  /// Returns true if the klass is effectively final (no subclass can exist
  /// at runtime). This includes final classes, type arrays, and object arrays
  /// whose element type is effectively final.
  using IsEffectivelyFinalFn = bool (*)(uintptr_t KlassPtr);
  IsEffectivelyFinalFn IsEffectivelyFinal = nullptr;
};

/// Register VM callbacks. Must be called before running the optimization
/// pipeline (typically once during compiler initialization).
void registerVMCallbacks(const VMCallbacks &CB);

/// Retrieve the registered VM callbacks. Returns nullptr if not registered.
const VMCallbacks *getVMCallbacks();

} // namespace llvm::jeandle

#endif // JEANDLE_VM_CALLBACK_H
