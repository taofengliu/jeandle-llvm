//===- SafepointUtils.h - Jeandle safepoint utilities ---------*- C++ -*-===//
//
// Copyright (c) 2026, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_JEANDLE_SAFEPOINTUTILS_H
#define LLVM_TRANSFORMS_JEANDLE_SAFEPOINTUTILS_H

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include <cstdint>
#include <optional>

namespace llvm {

class Function;
class BasicBlock;
class Instruction;
class CallInst;
class Loop;
class LoopInfo;
class MemorySSA;
class PHINode;
class SCEV;
class SCEVAddRecExpr;
class ScalarEvolution;
class Value;
class DominatorTree;

namespace jeandle {

bool isSafepointPoll(const Instruction &I);
bool isSafepoint(const Instruction &I);
bool isGuaranteedSafepointCall(const Instruction &I);

// The source poll and the continuing backedge observe the same memory state.
bool canRelocateSafepointPoll(CallInst &Poll, Loop &L, MemorySSA &MSSA);

uint64_t getLoopStripMiningIter();
bool isSafepointEliminationEnabled();
bool isStripMiningEnabled();
bool isInclusiveLoopVersioningEnabled();

bool backedgeCountProvablyLessThan(Loop &L, uint64_t ExclusiveLimit,
                                   ScalarEvolution &SE);

struct LoopSafepointFacts {
  bool IsWithinBudget = false;
  bool IsWithinIntBackedgeLimit = false;

  static LoopSafepointFacts get(Loop &L, ScalarEvolution &SE);
};

using RequiredPolls = SmallPtrSet<CallInst *, 8>;

// A function-pass invocation owns the budget; CFG mutations discard the
// epoch-local caches without replenishing the total work allowance.
class SafepointWorkBudget {
  uint64_t Remaining;
  bool Exhausted = false;

public:
  SafepointWorkBudget();
  bool spend(uint64_t Work = 1);
  bool checkDepth(Loop &L);
  bool exhausted() const { return Exhausted; }
};

class SafepointLoopAnalysis {
  struct Coverage {
    bool Calls = false;
    bool Covered = false;
  };
  DenseMap<Loop *, Coverage> CoverageCache;
  DenseMap<Loop *, LoopSafepointFacts> FactsCache;
  DenseMap<Loop *, bool> EmptyCache;
  Coverage &getCoverage(Loop &L);

public:
  LoopInfo &LI;
  DominatorTree &DT;
  ScalarEvolution &SE;
  SafepointWorkBudget &Work;
  SafepointLoopAnalysis(LoopInfo &LI, DominatorTree &DT, ScalarEvolution &SE,
                        SafepointWorkBudget &Work)
      : LI(LI), DT(DT), SE(SE), Work(Work) {}
  bool checkLoop(Loop &L);
  bool hasCoverage(Loop &L, bool CallsOnly = false);
  // Numerical exemptions and all-path coverage use the current loop shape.
  bool isExempt(Loop &L);
  bool hasIndependentCoverage(Loop &L, const RequiredPolls &Excluded);
  // Grow a whole-tree retention plan without changing the IR. Failure makes
  // the entire deletion plan unusable, including earlier successful loops.
  bool retainPolls(Loop &L, RequiredPolls &Retained);
  LoopSafepointFacts getFacts(Loop &L);
  bool isEmptyLoopCandidate(Loop &L);
};

// Polls selected on ancestor backedge paths. An incomplete analysis requires
// every poll to survive; consumers must not use partial negative results.
struct AncestorPollRequirements {
  RequiredPolls Polls;
  bool Complete = true;
  bool isRequired(const CallInst &Poll, DominatorTree &DT) const;
};

AncestorPollRequirements
computeAncestorPollRequirements(Loop &L, SafepointLoopAnalysis &Analysis);

// Phase 2 (C2 remove_safepoints keep_one). The latch-closest poll this loop
// owns on the dominator chain, preferring one an ancestor requires (it must
// survive anyway). Returns null if no single poll dominates every back-edge
// path.
CallInst *findKeepOne(Loop &L, LoopInfo &LI, DominatorTree &DT,
                      const RequiredPolls &Required);

// A safepoint poll that SafepointStripMining relocated onto the outer
// back-edge of a strip-mined nest: a jeandle.safepoint_poll call carrying the
// jeandle.strip-mined-poll call-site attribute.
bool isStripMinedPoll(const Instruction &I);

// Locate a candidate nest; this attribute is not a bound proof.
bool isMarkedStripMinedInner(Loop &L);

// Revalidate the current counter or counted clamp and the outer poll.
bool isBoundedStripMinedInner(Loop &L, ScalarEvolution &SE, DominatorTree &DT);

constexpr unsigned StripMineStrideOverflowWidenBits = 64;

std::optional<APInt> getConstantAddStep(Value *V, PHINode *Phi);

bool canProveExclusiveNoWrap(const SCEVAddRecExpr *AR, const APInt &Step,
                             const SCEV *LimitS, const Instruction *CtxI,
                             bool Signed, bool Increasing, ScalarEvolution &SE);

bool canProveInclusiveNoWrap(const SCEVAddRecExpr *AR, const APInt &Step,
                             Value *Limit, const SCEV *LimitS,
                             const Instruction *CtxI, bool Signed,
                             bool Increasing, ScalarEvolution &SE);

} // namespace jeandle
} // namespace llvm

#endif // LLVM_TRANSFORMS_JEANDLE_SAFEPOINTUTILS_H
