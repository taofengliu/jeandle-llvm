//===- JavaType.h - Java Type Query Interface -----------------------------===//
//
// Copyright (c) 2025, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Provides interfaces to query Java type information from LLVM IR.
// Type information is encoded in attributes ("java-klass", "java-klass-exact")
// and metadata (!java-klass). This interface can be used by any Jeandle pass
// that needs compile-time type information.
//
//===----------------------------------------------------------------------===//

#ifndef JEANDLE_JAVA_TYPE_H
#define JEANDLE_JAVA_TYPE_H

#include <cstdint>

namespace llvm {

class DominatorTree;
class Instruction;
class Value;

namespace jeandle {

/// Represents the Java type of an LLVM IR value.
struct JavaType {
  /// The Klass pointer (from HotSpot JVM). 0 means unknown.
  uintptr_t Klass = 0;

  /// If true, the value is exactly this class, not a subclass.
  bool Exact = false;

  bool isUnknown() const { return Klass == 0; }
  bool isKnown() const { return Klass != 0; }
};

/// Get the Java type of a value.
///
/// When Context is null, performs a context-insensitive query using only
/// attributes and metadata attached to the value (and PHI incoming values).
///
/// When Context is provided, additionally performs context-sensitive sharpening
/// by examining dominating type checks (jeandle.check_instanceof calls) that
/// constrain the value's type at the point of the context instruction.
///
/// The query handles arbitrary IR shapes including PHI nodes (with cycle
/// detection), select instructions, casts, and various comparison patterns
/// for branch conditions.
JavaType getJavaType(Value *V, DominatorTree &DT,
                     Instruction *Context = nullptr);

/// Compute the lowest common ancestor (LCA) of two Java types.
/// Returns unknown if either input is unknown or if the LCA cannot be
/// determined.
JavaType computeLCA(JavaType A, JavaType B);

/// Extract a Klass pointer constant from a Value.
/// Handles inttoptr of ConstantInt and ConstantExpr patterns.
/// Returns 0 if the value does not encode a constant klass pointer.
uintptr_t extractKlassConstant(Value *V);

} // namespace jeandle
} // namespace llvm

#endif // JEANDLE_JAVA_TYPE_H
