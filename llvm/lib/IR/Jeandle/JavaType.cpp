//===- JavaType.cpp - Java Type Query Implementation ----------------------===//
//
// Copyright (c) 2025, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/Jeandle/JavaType.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Jeandle/Attributes.h"
#include "llvm/IR/Jeandle/Metadata.h"
#include "llvm/IR/Jeandle/VMCallback.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "java-type"

using namespace llvm;
using namespace llvm::jeandle;

// =============================================================================
// Helpers
// =============================================================================

uintptr_t jeandle::extractKlassConstant(Value *V) {
  if (auto *I2P = dyn_cast<IntToPtrInst>(V)) {
    if (auto *CI = dyn_cast<ConstantInt>(I2P->getOperand(0)))
      return CI->getZExtValue();
  }
  if (auto *CE = dyn_cast<ConstantExpr>(V)) {
    if (CE->getOpcode() == Instruction::IntToPtr) {
      if (auto *CI = dyn_cast<ConstantInt>(CE->getOperand(0)))
        return CI->getZExtValue();
    }
  }
  return 0;
}

/// Return true if F is jeandle.check_instanceof.
static bool isCheckInstanceofFn(const Function *F) {
  return F && F->getName() == "jeandle.check_instanceof";
}

/// If CI is a call to jeandle.check_instanceof, return the super klass and obj.
static bool isCheckInstanceofCall(const CallInst *CI, uintptr_t &Klass,
                                  Value *&Obj) {
  if (!isCheckInstanceofFn(CI->getCalledFunction()))
    return false;
  Klass = extractKlassConstant(CI->getArgOperand(0));
  Obj = CI->getArgOperand(1);
  return Klass != 0;
}

/// Check if klass K is excluded by set S, meaning there exists Y in S such
/// that IsSubtype(K, Y). Excluding Y implies excluding all subtypes of Y.
static bool isExcludedBy(uintptr_t K, const SmallDenseSet<uintptr_t, 2> &S) {
  const VMCallbacks *CB = getVMCallbacks();
  assert(CB && CB->IsSubtype && "VMCallbacks must be set");
  for (uintptr_t Y : S) {
    if (CB->IsSubtype(K, Y))
      return true;
  }
  return false;
}

/// Add an excluded klass to the set, maintaining the invariant that only the
/// most general (uppermost) excluded classes are stored.
static void addExcludedKlass(SmallDenseSet<uintptr_t, 2> &Set, uintptr_t K) {
  const VMCallbacks *CB = getVMCallbacks();
  assert(CB && CB->IsSubtype && "VMCallbacks must be set");

  // If K is already covered by a more general exclusion, skip.
  if (isExcludedBy(K, Set))
    return;

  // Remove any existing entries that are more specific than K.
  SmallVector<uintptr_t, 2> ToRemove;
  for (uintptr_t Y : Set) {
    if (CB->IsSubtype(Y, K))
      ToRemove.push_back(Y);
  }
  for (uintptr_t Y : ToRemove)
    Set.erase(Y);

  Set.insert(K);
}

/// Compute the subtype-aware intersection of two ExcludedKlasses sets.
/// A klass survives if it is excluded by BOTH sets.
static SmallDenseSet<uintptr_t, 2>
intersectExcludedKlasses(const SmallDenseSet<uintptr_t, 2> &A,
                         const SmallDenseSet<uintptr_t, 2> &B) {
  SmallDenseSet<uintptr_t, 2> Result;
  for (uintptr_t X : A) {
    if (isExcludedBy(X, B))
      addExcludedKlass(Result, X);
  }
  for (uintptr_t Y : B) {
    if (isExcludedBy(Y, A))
      addExcludedKlass(Result, Y);
  }
  return Result;
}

/// Merge ExcludedKlasses from Src into Dst (union). Both sets of negative
/// constraints apply to the same value at the same point.
static void unionExcludedKlasses(SmallDenseSet<uintptr_t, 2> &Dst,
                                 const SmallDenseSet<uintptr_t, 2> &Src) {
  for (uintptr_t K : Src)
    addExcludedKlass(Dst, K);
}

/// Enforce ExcludedKlasses invariants:
/// 1. If Exact, clear ExcludedKlasses (type is fully determined).
/// 2. If Klass is known, remove excluded klasses that are not subtypes of
///    Klass (they are already impossible).
static void normalizeExcludedKlasses(JavaType &T) {
  if (T.ExcludedKlasses.empty())
    return;
  if (T.Exact) {
    T.ExcludedKlasses.clear();
    return;
  }
  if (T.Klass != 0) {
    const VMCallbacks *CB = getVMCallbacks();
    assert(CB && CB->IsSubtype && "VMCallbacks must be set");
    SmallVector<uintptr_t, 2> ToRemove;
    for (uintptr_t K : T.ExcludedKlasses) {
      if (!CB->IsSubtype(K, T.Klass))
        ToRemove.push_back(K);
    }
    for (uintptr_t K : ToRemove)
      T.ExcludedKlasses.erase(K);
  }
}

JavaType jeandle::typeUnion(JavaType A, JavaType B) {
  if (A.Klass == 0 && B.Klass == 0) {
    // Both have unknown positive type. Intersect exclusions.
    JavaType Result;
    if (!A.ExcludedKlasses.empty() && !B.ExcludedKlasses.empty())
      Result.ExcludedKlasses =
          intersectExcludedKlasses(A.ExcludedKlasses, B.ExcludedKlasses);
    return Result;
  }
  if (A.Klass == 0 || B.Klass == 0)
    return {}; // One known, one unknown positive type → unknown.
  JavaType Result;
  if (A.Klass == B.Klass) {
    Result.Klass = A.Klass;
    Result.Exact = A.Exact && B.Exact;
  } else {
    const VMCallbacks *CB = getVMCallbacks();
    assert(CB && CB->GetCommonSuperKlass && "VMCallbacks must be set");
    uintptr_t LCA = CB->GetCommonSuperKlass(A.Klass, B.Klass);
    if (LCA == 0)
      return {};
    Result.Klass = LCA;
    Result.Exact = false;
  }
  // Intersect exclusions (value could be either A or B).
  if (!A.ExcludedKlasses.empty() && !B.ExcludedKlasses.empty())
    Result.ExcludedKlasses =
        intersectExcludedKlasses(A.ExcludedKlasses, B.ExcludedKlasses);
  normalizeExcludedKlasses(Result);
  return Result;
}

JavaType jeandle::typeIntersect(JavaType A, JavaType B) {
  JavaType Result;

  // Positive type: pick the more specific one.
  if (A.isKnown() && B.isKnown()) {
    const VMCallbacks *CB = getVMCallbacks();
    assert(CB && CB->IsSubtype && "VMCallbacks must be set");
    if (CB->IsSubtype(A.Klass, B.Klass)) {
      Result.Klass = A.Klass;
      Result.Exact = A.Exact;
    } else if (CB->IsSubtype(B.Klass, A.Klass)) {
      Result.Klass = B.Klass;
      Result.Exact = B.Exact;
    }
    // else: neither is a subtype of the other — contradictory constraints
    // (dead code). Leave Result.Klass = 0 (unknown).
  } else if (A.isKnown()) {
    Result.Klass = A.Klass;
    Result.Exact = A.Exact;
  } else if (B.isKnown()) {
    Result.Klass = B.Klass;
    Result.Exact = B.Exact;
  }

  // Negative constraints: union (both exclusions apply at the same point).
  Result.ExcludedKlasses = A.ExcludedKlasses;
  unionExcludedKlasses(Result.ExcludedKlasses, B.ExcludedKlasses);
  normalizeExcludedKlasses(Result);

  return Result;
}

// =============================================================================
// Context-insensitive type query
// =============================================================================

static JavaType getBaseJavaType(Value *V,
                                SmallPtrSetImpl<const PHINode *> &Visited) {
  // Argument: check param attributes.
  if (auto *Arg = dyn_cast<Argument>(V)) {
    const Function *F = Arg->getParent();
    unsigned Idx = Arg->getArgNo();
    const AttributeList &AL = F->getAttributes();
    if (AL.hasParamAttr(Idx, jeandle::Attribute::JavaKlass)) {
      StringRef KlassStr = AL.getParamAttr(Idx, jeandle::Attribute::JavaKlass)
                               .getValueAsString();
      uintptr_t Klass = 0;
      if (!KlassStr.getAsInteger(10, Klass) && Klass != 0) {
        bool Exact = AL.hasParamAttr(Idx, jeandle::Attribute::JavaKlassExact);
        return {Klass, Exact};
      }
    }
    return {};
  }

  // Call / Invoke: check return attributes.
  if (auto *CB = dyn_cast<CallBase>(V)) {
    const AttributeList &AL = CB->getAttributes();
    if (AL.hasRetAttr(jeandle::Attribute::JavaKlass)) {
      StringRef KlassStr = AL.getAttributeAtIndex(AttributeList::ReturnIndex,
                                                  jeandle::Attribute::JavaKlass)
                               .getValueAsString();
      uintptr_t Klass = 0;
      if (!KlassStr.getAsInteger(10, Klass) && Klass != 0) {
        bool Exact = AL.hasRetAttr(jeandle::Attribute::JavaKlassExact);
        return {Klass, Exact};
      }
    }
    return {};
  }

  // Load: check !java-klass metadata.
  if (auto *LI = dyn_cast<LoadInst>(V)) {
    if (MDNode *MD = LI->getMetadata(jeandle::Metadata::JavaKlass)) {
      if (MD->getNumOperands() >= 1) {
        if (auto *CMD = dyn_cast<ConstantAsMetadata>(MD->getOperand(0))) {
          if (auto *CI = dyn_cast<ConstantInt>(CMD->getValue())) {
            uintptr_t Klass = CI->getZExtValue();
            if (Klass != 0) {
              bool Exact =
                  LI->getMetadata(jeandle::Metadata::JavaKlassExact) != nullptr;
              return {Klass, Exact};
            }
          }
        }
      }
    }
    return {};
  }

  // PHI: compute LCA of all incoming values.
  // For cycles (loop back-edges): when an incoming is a PHI already in the
  // visited set, skip it. The type is determined by the non-cyclic incomings.
  if (auto *PN = dyn_cast<PHINode>(V)) {
    if (!Visited.insert(PN).second)
      return {}; // Cycle detected — caller will skip this incoming.
    JavaType Result;
    bool First = true;
    for (unsigned I = 0, E = PN->getNumIncomingValues(); I != E; ++I) {
      Value *Inc = PN->getIncomingValue(I);
      if (auto *IncPN = dyn_cast<PHINode>(Inc)) {
        if (Visited.count(IncPN))
          continue; // Skip cyclic incoming.
      }
      JavaType IncType = getBaseJavaType(Inc, Visited);
      if (IncType.isUnknown())
        return {};
      if (First) {
        Result = IncType;
        First = false;
      } else {
        Result = typeUnion(Result, IncType);
        if (Result.isUnknown())
          return {};
      }
    }
    return Result;
  }

  // Select: LCA of both operands.
  if (auto *SI = dyn_cast<SelectInst>(V)) {
    JavaType TrueType = getBaseJavaType(SI->getTrueValue(), Visited);
    if (TrueType.isUnknown())
      return {};
    JavaType FalseType = getBaseJavaType(SI->getFalseValue(), Visited);
    return typeUnion(TrueType, FalseType);
  }

  // BitCast / AddrSpaceCast: pass through.
  if (auto *BC = dyn_cast<BitCastInst>(V))
    return getBaseJavaType(BC->getOperand(0), Visited);
  if (auto *ASC = dyn_cast<AddrSpaceCastInst>(V))
    return getBaseJavaType(ASC->getOperand(0), Visited);

  return {};
}

// =============================================================================
// Condition tracing: trace from a branch condition to a check_instanceof call
// =============================================================================

namespace {

/// Result of tracing a branch condition back to a jeandle.check_instanceof
/// call.
///
/// Given a conditional branch `br i1 %cond, label %true_bb, label %false_bb`,
/// traceToCheckInstanceof attempts to determine whether %cond is derived from
/// a check_instanceof on a specific object. If successful:
///
///   - Klass: the super-klass pointer that was checked against.
///   - Negated: whether the branch semantics are inverted. When false, the
///     true-successor is the "type confirmed" path (the check returned true).
///     When true, the false-successor is the "type confirmed" path (e.g., the
///     condition was `icmp eq i32 %instanceof_result, 0`, so true means the
///     check failed, and false means it succeeded).
///
/// A Klass of 0 means the trace did not find a matching check_instanceof.
struct TraceResult {
  uintptr_t Klass = 0;
  bool Negated = false;
  bool PositiveOnly = false; // Klass is only valid for positive sharpening,
                             // not for negative (type-denied) constraints.
                             // Set when klass was computed via LCA from
                             // incomings with different klasses.

  bool matched() const { return Klass != 0; }
};

} // anonymous namespace

/// Check if IncomingBB is reached only when Obj is null.
/// Walks up the dominator tree from IncomingBB looking for a conditional branch
/// on `icmp eq/ne Obj, null` where IncomingBB is dominated by the "Obj is null"
/// successor. Using the dominator tree handles all CFG shapes (diamonds, etc.),
/// not just single-predecessor chains.
static bool isNullCheckPath(BasicBlock *IncomingBB, Value *Obj,
                            DominatorTree &DT) {
  for (auto *Node = DT.getNode(IncomingBB); Node; Node = Node->getIDom()) {
    BasicBlock *BB = Node->getBlock();
    auto *BI = dyn_cast<BranchInst>(BB->getTerminator());
    if (!BI || !BI->isConditional())
      continue;

    auto *Cmp = dyn_cast<ICmpInst>(BI->getCondition());
    if (!Cmp)
      continue;

    // Check if condition is `icmp eq/ne Obj, null`.
    Value *LHS = Cmp->getOperand(0);
    Value *RHS = Cmp->getOperand(1);
    bool LHSNull = isa<ConstantPointerNull>(LHS);
    bool RHSNull = isa<ConstantPointerNull>(RHS);
    Value *Tested = LHSNull ? RHS : (RHSNull ? LHS : nullptr);
    if (Tested != Obj)
      continue;

    // Determine which successor means "Obj is null".
    // icmp eq Obj, null → true successor = null path
    // icmp ne Obj, null → false successor = null path
    BasicBlock *NullBB = nullptr;
    if (Cmp->getPredicate() == ICmpInst::ICMP_EQ)
      NullBB = BI->getSuccessor(0);
    else if (Cmp->getPredicate() == ICmpInst::ICMP_NE)
      NullBB = BI->getSuccessor(1);
    else
      continue;

    // IncomingBB must be dominated by the null-path successor.
    return DT.dominates(NullBB, IncomingBB);
  }
  return false;
}

/// Recursively trace a branch condition back to a jeandle.check_instanceof
/// call on QueryObj. Returns a matched TraceResult if successful, or an
/// unmatched TraceResult ({Klass=0}) if the condition cannot be linked to a
/// check_instanceof on QueryObj.
static TraceResult traceToCheckInstanceof(Value *Cond, Value *QueryObj,
                                          SmallPtrSetImpl<Value *> &Visited,
                                          DominatorTree &DT) {
  // Avoid infinite recursion on cyclic value graphs.
  if (!Visited.insert(Cond).second)
    return {}; // Already visited — no match on this path.

  // --- Base case: direct call to jeandle.check_instanceof(klass, obj) ---
  if (auto *CI = dyn_cast<CallInst>(Cond)) {
    uintptr_t Klass = 0;
    Value *Obj = nullptr;
    if (isCheckInstanceofCall(CI, Klass, Obj) && Obj == QueryObj)
      return {Klass, false}; // Direct match, not negated.
    return {};               // Not a check_instanceof on QueryObj.
  }

  // --- ICmp: comparisons that test the result of a type check ---
  //
  // The value being compared ultimately derives from a check_instanceof, which
  // returns i1 (0 or 1). It may have been widened (e.g., zext i1 to i32), but
  // the only meaningful values are 0 (check failed) and 1 (check passed).
  //
  // Rather than matching specific predicates (eq, ne) against specific
  // constants (0, 1), we use a general approach: evaluate what the comparison
  // returns for each possible input (0 and 1), then determine whether it
  // discriminates between "check passed" and "check failed".
  //
  // Examples of how this works:
  //
  //   `icmp ne i32 %val, 0`:
  //     val=0 → ne(0,0) → false    val=1 → ne(1,0) → true
  //     Discriminating: true when check passed → not negated.
  //
  //   `icmp eq i32 %val, 0`:
  //     val=0 → eq(0,0) → true     val=1 → eq(1,0) → false
  //     Discriminating: true when check failed → negated.
  //
  //   `icmp sgt i32 %val, 0`:
  //     val=0 → sgt(0,0) → false   val=1 → sgt(1,0) → true
  //     Discriminating: true when check passed → not negated.
  //
  //   `icmp uge i32 %val, 1`:
  //     val=0 → uge(0,1) → false   val=1 → uge(1,1) → true
  //     Discriminating: true when check passed → not negated.
  //
  //   `icmp sge i32 %val, 0`:
  //     val=0 → sge(0,0) → true    val=1 → sge(1,0) → true
  //     Both true → always true, not discriminating → no match.
  //
  if (auto *Cmp = dyn_cast<ICmpInst>(Cond)) {
    Value *LHS = Cmp->getOperand(0);
    Value *RHS = Cmp->getOperand(1);
    auto Pred = Cmp->getPredicate();

    // Normalize so the constant is always on the RHS, adjusting the predicate
    // accordingly. E.g., `icmp sgt 0, %val` becomes `icmp slt %val, 0`.
    Value *Val = LHS;
    ConstantInt *C = dyn_cast<ConstantInt>(RHS);
    if (!C) {
      C = dyn_cast<ConstantInt>(LHS);
      Val = RHS;
      Pred = ICmpInst::getSwappedPredicate(Pred);
    }
    if (C) {
      // Evaluate `icmp Pred %val, C` for the two possible boolean inputs.
      unsigned BitWidth = C->getType()->getIntegerBitWidth();
      APInt ZeroVal(BitWidth, 0);
      APInt OneVal(BitWidth, 1);
      bool ResultForZero = ICmpInst::compare(ZeroVal, C->getValue(), Pred);
      bool ResultForOne = ICmpInst::compare(OneVal, C->getValue(), Pred);

      if (ResultForZero != ResultForOne) {
        // The comparison discriminates between 0 and 1 — it tells us whether
        // the underlying check_instanceof passed or failed. Now trace the
        // non-constant operand to find that check_instanceof.
        TraceResult R = traceToCheckInstanceof(Val, QueryObj, Visited, DT);
        if (!R.matched())
          return {}; // Val doesn't trace to a check — no match.
        if (ResultForOne)
          // The comparison returns true when val=1 (check passed).
          // On the true-branch, the check passed → same negation as inner.
          return R;
        else
          // The comparison returns true when val=0 (check failed).
          // On the true-branch, the check FAILED → flip negation.
          return {R.Klass, !R.Negated, R.PositiveOnly};
      }
      // ResultForZero == ResultForOne: the comparison is always true or always
      // false regardless of the check result (e.g., `sge %val, 0` is always
      // true for {0,1}). It provides no type information.
    }
    return {}; // No constant operand, or not discriminating — no match.
  }

  // --- ZExt / SExt / Trunc: transparent casts, trace through to source ---
  // (e.g., `zext i1 %check_instanceof_result to i32`)
  if (auto *Cast = dyn_cast<CastInst>(Cond)) {
    if (isa<ZExtInst>(Cast) || isa<SExtInst>(Cast) || isa<TruncInst>(Cast))
      return traceToCheckInstanceof(Cast->getOperand(0), QueryObj, Visited, DT);
    return {}; // Other casts (bitcast, fpcast, ...) — not meaningful here.
  }

  // --- And i1 %a, %b: on the true-branch both operands are true ---
  // Both constraints hold simultaneously, so if both trace to a check on
  // QueryObj, we pick the more specific klass (tighter bound). If only one
  // matches, that single constraint is still valid.
  if (auto *BO = dyn_cast<BinaryOperator>(Cond)) {
    if (BO->getOpcode() == Instruction::And) {
      TraceResult L =
          traceToCheckInstanceof(BO->getOperand(0), QueryObj, Visited, DT);
      TraceResult R =
          traceToCheckInstanceof(BO->getOperand(1), QueryObj, Visited, DT);
      if (L.matched() && R.matched()) {
        // Both matched — pick the more specific klass.
        const VMCallbacks *CB = getVMCallbacks();
        assert(CB && CB->IsSubtype && "VMCallbacks must be set");
        bool PO = L.PositiveOnly || R.PositiveOnly;
        if (CB->IsSubtype(L.Klass, R.Klass))
          return {L.Klass, L.Negated, PO};
        if (CB->IsSubtype(R.Klass, L.Klass))
          return {R.Klass, R.Negated, PO};
        return {L.Klass, L.Negated, PO}; // Unrelated — either is valid.
      }
      if (L.matched())
        return L; // Only left matched.
      return R;   // Right matched, or no match at all.
    }
    return {}; // Other binary ops (or, xor, ...) — not handled.
  }

  // --- Select: the result is one of two values, we don't know which ---
  // On the true-branch of `br i1 (select %c, %a, %b)`, the selected value
  // is true — but it could be either %a or %b. So we can only guarantee
  // what holds for BOTH possibilities, i.e., the LCA of the two klasses.
  if (auto *SI = dyn_cast<SelectInst>(Cond)) {
    TraceResult T =
        traceToCheckInstanceof(SI->getTrueValue(), QueryObj, Visited, DT);
    if (!T.matched())
      return {}; // True arm doesn't trace — no match.
    TraceResult F =
        traceToCheckInstanceof(SI->getFalseValue(), QueryObj, Visited, DT);
    if (!F.matched())
      return {}; // False arm doesn't trace — no match.
    if (T.Negated != F.Negated)
      return {}; // Conflicting negation — cannot combine.
    if (T.Klass == F.Klass) {
      // Same klass — propagate PositiveOnly if either arm has it.
      return {T.Klass, T.Negated, T.PositiveOnly || F.PositiveOnly};
    }
    // Different klasses: compute LCA (the least specific common supertype).
    // LCA is only valid for positive sharpening — failing one check doesn't
    // mean the object isn't the LCA type.
    const VMCallbacks *CB = getVMCallbacks();
    assert(CB && CB->GetCommonSuperKlass && "VMCallbacks must be set");
    uintptr_t LCA = CB->GetCommonSuperKlass(T.Klass, F.Klass);
    if (LCA != 0)
      return {LCA, T.Negated, /*PositiveOnly=*/true};
    return {};                 // Cannot determine LCA — no match.
  }

  // --- PHI: compute LCA of non-constant incomings ---
  // This handles merged control flow where different predecessors computed
  // type check results (e.g., inlined instanceof with a null-check PHI).
  // When incomings have different klasses, the LCA is a valid positive bound
  // but must NOT be used for negative constraints (failing one check doesn't
  // mean the object isn't the LCA type).
  if (auto *PN = dyn_cast<PHINode>(Cond)) {
    const VMCallbacks *CB = getVMCallbacks();
    assert(CB && CB->GetCommonSuperKlass && "VMCallbacks must be set");

    uintptr_t RunningLCA = 0;
    bool MatchedNegated = false;
    bool HaveMatch = false;
    bool PositiveOnly = false;

    for (unsigned I = 0, E = PN->getNumIncomingValues(); I != E; ++I) {
      Value *Inc = PN->getIncomingValue(I);

      if (auto *CI = dyn_cast<ConstantInt>(Inc)) {
        // Constant false/zero: this path contributes "check failed" to the
        // PHI. On the true-branch of `br i1 (ne phi, 0)`, this path could
        // not have been taken, so it is safe to ignore.
        if (CI->isZero())
          continue;
        // Constant true/non-zero: this path contributes "check passed"
        // WITHOUT an actual check_instanceof. This is safe to ignore ONLY
        // if the incoming is from a null-check path on QueryObj — since
        // we assume queried value is non-null. (Type of a null pointer
        // is meaningless)
        if (isNullCheckPath(PN->getIncomingBlock(I), QueryObj, DT))
          continue;
        // Constant true from an unknown origin: on the true-branch, this
        // path could have been taken without any type check, so we cannot
        // conclude anything about QueryObj's type.
        return {};
      }

      // Non-constant incoming: must trace to a check_instanceof.
      TraceResult R = traceToCheckInstanceof(Inc, QueryObj, Visited, DT);
      if (!R.matched())
        return {}; // This incoming doesn't trace to a check — no match.
      if (!HaveMatch) {
        RunningLCA = R.Klass;
        MatchedNegated = R.Negated;
        PositiveOnly = R.PositiveOnly;
        HaveMatch = true;
      } else {
        if (R.Negated != MatchedNegated)
          return {}; // Different negation — cannot combine.
        if (R.Klass != RunningLCA) {
          RunningLCA = CB->GetCommonSuperKlass(RunningLCA, R.Klass);
          if (RunningLCA == 0)
            return {};
          PositiveOnly = true;
        }
        if (R.PositiveOnly)
          PositiveOnly = true; // Propagate from any incoming.
      }
    }
    if (HaveMatch)
      return {RunningLCA, MatchedNegated, PositiveOnly};
    return {}; // No non-constant incomings (all were skipped) — no match.
  }

  return {}; // Unrecognized value kind — no match.
}

// =============================================================================
// Context-sensitive sharpening
// =============================================================================

/// DestBB: for PHI incoming processing, the PHI's parent block. When provided,
/// the incoming block's own branch is considered for sharpening (the branch
/// targets DestBB, so it should be considered to sharpen the PHI's type).
/// For non-PHI contexts, DestBB is nullptr and the context block's own branch
/// is skipped.
static JavaType sharpenFromDominators(Value *V, Instruction *Context,
                                      DominatorTree &DT,
                                      BasicBlock *DestBB = nullptr) {
  const VMCallbacks *CB = getVMCallbacks();
  assert(CB && CB->IsSubtype && "VMCallbacks must be set");

  BasicBlock *ContextBB = Context->getParent();
  JavaType Best;

  for (auto *Node = DT.getNode(ContextBB); Node; Node = Node->getIDom()) {
    BasicBlock *BB = Node->getBlock();
    auto *BI = dyn_cast<BranchInst>(BB->getTerminator());
    if (!BI || !BI->isConditional())
      continue;

    // For ContextBB's own branch: skip unless DestBB is provided (PHI case).
    // The branch hasn't executed for non-PHI contexts, but for PHI incomings
    // the branch targets DestBB, so it should be considered for sharpening.
    if (BB == ContextBB && !DestBB)
      continue;

    SmallPtrSet<Value *, 16> TraceVisited;
    TraceResult TR =
        traceToCheckInstanceof(BI->getCondition(), V, TraceVisited, DT);
    if (!TR.matched())
      continue;

    // Determine the type-confirmed and type-denied successors.
    BasicBlock *TypeConfirmedBB =
        TR.Negated ? BI->getSuccessor(1) : BI->getSuccessor(0);
    BasicBlock *TypeDeniedBB =
        TR.Negated ? BI->getSuccessor(0) : BI->getSuccessor(1);

    // For ContextBB's own branch, check against DestBB (the PHI's block).
    // For dominator blocks above ContextBB, check against ContextBB as before.
    BasicBlock *CheckBB = (BB == ContextBB) ? DestBB : ContextBB;

    if (DT.dominates(TypeConfirmedBB, CheckBB)) {
      // V is a subtype of TR.Klass at Context (positive constraint).
      LLVM_DEBUG(dbgs() << "JavaType: sharpened " << *V << " to klass "
                        << TR.Klass << " from dominating check in "
                        << BB->getName() << "\n");

      if (!Best.isKnown()) {
        Best.Klass = TR.Klass;
        Best.Exact = false;
      } else {
        // Keep the more specific type.
        if (CB->IsSubtype(TR.Klass, Best.Klass)) {
          Best.Klass = TR.Klass;
          Best.Exact = false;
        }
        // else Best is already more specific, keep it.
      }
    } else if (DT.dominates(TypeDeniedBB, CheckBB) && !TR.PositiveOnly) {
      // V is NOT an instance of TR.Klass at Context (negative constraint).
      LLVM_DEBUG(dbgs() << "JavaType: excluded " << *V << " from klass "
                        << TR.Klass << " from dominating failed check in "
                        << BB->getName() << "\n");
      addExcludedKlass(Best.ExcludedKlasses, TR.Klass);
    }
  }

  normalizeExcludedKlasses(Best);
  return Best;
}

// =============================================================================
// Main query
// =============================================================================

static JavaType getJavaTypeImpl(Value *V, DominatorTree &DT,
                                Instruction *Context,
                                SmallPtrSetImpl<const PHINode *> &Visited,
                                BasicBlock *DestBB = nullptr);

/// Context-sensitive PHI handling: query each incoming with its own context.
/// For PHI cycles (loop back-edges): when we re-encounter a PHI already in the
/// visited set, we skip that incoming. The type is determined only by the
/// non-cyclic incomings. This is sound because a loop PHI's type is the LCA of
/// all values entering the cycle, which are exactly the non-cyclic incomings.
static JavaType getPhiJavaType(PHINode *PN, DominatorTree &DT,
                               SmallPtrSetImpl<const PHINode *> &Visited) {
  if (!Visited.insert(PN).second)
    return {}; // Cycle detected — caller will skip this incoming.

  JavaType Result;
  bool First = true;
  for (unsigned I = 0, E = PN->getNumIncomingValues(); I != E; ++I) {
    Value *Inc = PN->getIncomingValue(I);
    BasicBlock *IncBB = PN->getIncomingBlock(I);
    Instruction *IncContext = IncBB->getTerminator();

    // Check if the incoming is a PHI we've already visited (cycle).
    // If so, skip it — the type from this path will be determined by the
    // non-cyclic incomings.
    if (auto *IncPN = dyn_cast<PHINode>(Inc)) {
      if (Visited.count(IncPN))
        continue;
    }

    JavaType IncType =
        getJavaTypeImpl(Inc, DT, IncContext, Visited, PN->getParent());
    if (IncType.isUnknown())
      return {};
    if (First) {
      Result = IncType;
      First = false;
    } else {
      Result = typeUnion(Result, IncType);
      if (Result.isUnknown())
        return {};
    }
  }
  return Result;
}

static JavaType getJavaTypeImpl(Value *V, DominatorTree &DT,
                                Instruction *Context,
                                SmallPtrSetImpl<const PHINode *> &Visited,
                                BasicBlock *DestBB) {
  // Context-sensitive PHI handling: compute per-incoming types via
  // getPhiJavaType, then also sharpen from dominators of the Context.
  // The PHI's incoming analysis gives the base type; dominator checks at
  // the use site (Context) can further narrow it.
  if (auto *PN = dyn_cast<PHINode>(V)) {
    if (Context) {
      JavaType PhiType = getPhiJavaType(PN, DT, Visited);
      JavaType Sharpened = sharpenFromDominators(V, Context, DT, DestBB);
      return typeIntersect(PhiType, Sharpened);
    }
  }

  // Get base type (context-insensitive).
  JavaType Base = getBaseJavaType(V, Visited);

  // Context-sensitive sharpening: intersect with dominator-derived constraints.
  if (Context) {
    JavaType Sharpened = sharpenFromDominators(V, Context, DT, DestBB);
    Base = typeIntersect(Base, Sharpened);
  }

  return Base;
}

JavaType jeandle::getJavaType(Value *V, DominatorTree &DT,
                              Instruction *Context) {
  SmallPtrSet<const PHINode *, 8> Visited;
  return getJavaTypeImpl(V, DT, Context, Visited);
}
