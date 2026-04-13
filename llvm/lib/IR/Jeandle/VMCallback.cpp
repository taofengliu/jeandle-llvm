//===- VMCallback.cpp - VM Callback Interface -----------------------------===//
//
// Copyright (c) 2025, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/Jeandle/VMCallback.h"

namespace llvm::jeandle {

static VMCallbacks StoredCallbacks;
static bool CallbacksRegistered = false;

void registerVMCallbacks(const VMCallbacks &CB) {
  StoredCallbacks = CB;
  CallbacksRegistered = true;
}

const VMCallbacks *getVMCallbacks() {
  return CallbacksRegistered ? &StoredCallbacks : nullptr;
}

} // namespace llvm::jeandle
