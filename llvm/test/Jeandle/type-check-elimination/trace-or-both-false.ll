; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/trace-or-both-false.cblog %s 2>&1 | FileCheck %s

; Test: `or i1 %checkA, %checkB` as branch condition.
; False-branch: both checks failed (AllOf semantics for negation).
; checkA: instanceof 6 (Animal), checkB: instanceof 10 (AbstractList).
; False-branch: excluded={6, 10}. Third check for Dog(7) is subtype of excluded 6.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @test(ptr addrspace(1) nonnull %obj) gc "hotspotgc" {
entry:
  %checkA = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 6 to ptr),
    ptr addrspace(1) nonnull %obj)
  %checkB = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 10 to ptr),
    ptr addrspace(1) nonnull %obj)
  %either = or i1 %checkA, %checkB
  br i1 %either, label %exit_true, label %neither

neither:
  %check3 = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 7 to ptr),
    ptr addrspace(1) nonnull %obj)
  ret i1 %check3

exit_true:
  ret i1 true
}

; CHECK-LABEL: @test
; CHECK: neither:
; CHECK-NEXT: ret i1 false

!java-method-compilation = !{}
