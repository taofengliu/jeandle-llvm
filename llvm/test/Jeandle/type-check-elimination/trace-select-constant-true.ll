; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/trace-select-constant-true.cblog %s 2>&1 | FileCheck %s

; Test: `select i1 %cond, i1 true, i1 %check` as branch condition.
; On false-branch: the constant true arm can't be selected (it would make cond true).
; So the non-constant arm (%check) was selected and is false.
; False-branch gets %check's FalseExclusions.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @test(ptr addrspace(1) nonnull %obj, i1 %cond) gc "hotspotgc" {
entry:
  %check1 = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 6 to ptr),
    ptr addrspace(1) nonnull %obj)
  %sel = select i1 %cond, i1 true, i1 %check1
  br i1 %sel, label %exit_true, label %not_animal

not_animal:
  %check2 = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 7 to ptr),
    ptr addrspace(1) nonnull %obj)
  ret i1 %check2

exit_true:
  ret i1 true
}

; CHECK-LABEL: @test
; CHECK: not_animal:
; CHECK-NEXT: ret i1 false

!java-method-compilation = !{}
