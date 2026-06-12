//===- JeandleUtils.hpp - Jeandle common utility definitions --------------===//
//
// Copyright (c) 2026, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef JEANDLE_UTILS_HPP
#define JEANDLE_UTILS_HPP

#include "llvm/IR/Jeandle/Metadata.h"
#include "llvm/IR/Type.h"

namespace llvm::jeandle {

/// Basic types used by HotSpot JVM, mirroring HotSpot's BasicType enum.
/// The numeric values must match the HotSpot definitions exactly.
enum HotspotBasicType {
  T_BOOLEAN = 4,
  T_CHAR = 5,
  T_FLOAT = 6,
  T_DOUBLE = 7,
  T_BYTE = 8,
  T_SHORT = 9,
  T_INT = 10,
  T_LONG = 11,
  T_OBJECT = 12,
  T_ARRAY = 13,
};

/// Returns true if \p Ty is a pointer in the Java heap address space,
/// i.e., it represents a Java object reference (oop).
inline bool isJavaOopType(Type *Ty) {
  auto *PT = dyn_cast<PointerType>(Ty);
  return PT && PT->getAddressSpace() == jeandle::AddrSpace::JavaHeapAddrSpace;
}

} // namespace llvm::jeandle

#endif // JEANDLE_UTILS_HPP
