//===- GCStrategy.h - Jeandle GC Strategy ---------------------------------===//
//
// Copyright (c) 2025, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef JEANDLE_METADATA_H
#define JEANDLE_METADATA_H

namespace llvm::jeandle {

class Metadata {
public:
  static constexpr const char *CurrentThread = "current-thread";

  static constexpr const char *StackPointer = "stack-pointer";

  static constexpr const char *HeapBase = "heap-base";

  static constexpr const char *JavaMethodCompilation =
      "java-method-compilation";

  static constexpr const char *StaticCallPatchSize = "static-call-patch-size";

  static constexpr const char *JavaKlass = "java-klass";

  static constexpr const char *JavaKlassExact = "java-klass-exact";

  static constexpr const char *InlineScopeID = "inline-scope-id";

  // Set by SafepointElimination on a poll that is its loop's required coverage;
  // the keep-one/collapse rules must not delete the last one so tagged.
  static constexpr const char *PollCoverage = "poll-coverage";

  // Set by strip mining on the inner latch to identify a poll-free inner loop
  // whose bound must be validated structurally by the coverage verifier. Branch
  // metadata is droppable by later passes (e.g. SimplifyCFG folding the latch),
  // so this marker is only useful between SafepointElimination and the verifier
  // that immediately follows it — do not move the verifier later in the
  // pipeline expecting this to survive.
  static constexpr const char *StripMined = "strip-mined";
};

enum AddrSpace : unsigned {
  CHeapAddrSpace = 0,
  JavaHeapAddrSpace = 1,
  TLSAddrSpace = 2,
  NarrowOopAddrSpace = 3
};

} // namespace llvm::jeandle

#endif // JEANDLE_METADATA_H
