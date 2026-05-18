; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/trace-or-partial.cblog %s 2>&1 | FileCheck %s

; Test: `or i1 %checkA, %unrelated` where only one side traces to a check.
; False-branch: Or being false means both are false, so the matched check is false.
; The matched check's FalseExclusions apply on the false-branch.
; True-branch: could be due to unmatched operand → unsound. No constraint.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)
declare i1 @some_condition()

define i1 @test(ptr addrspace(1) nonnull %obj) gc "hotspotgc" {
entry:
  %checkA = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 6 to ptr),
    ptr addrspace(1) nonnull %obj)
  %other = call i1 @some_condition()
  %either = or i1 %checkA, %other
  br i1 %either, label %exit_true, label %neither

neither:
  %check2 = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 7 to ptr),
    ptr addrspace(1) nonnull %obj)
  ret i1 %check2

exit_true:
  ret i1 true
}

; CHECK-LABEL: @test
; CHECK: neither:
; CHECK-NEXT: ret i1 false

!java-method-compilation = !{}
