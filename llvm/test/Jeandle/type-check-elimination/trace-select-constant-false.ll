; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/trace-select-constant-false.cblog %s 2>&1 | FileCheck %s

; Test: `select i1 %cond, i1 %check, i1 false` as branch condition.
; On true-branch: the constant false arm can't be selected → the non-constant arm
; (%check) was selected and is true. So true-branch has %check's TrueKlass.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @test(ptr addrspace(1) nonnull %obj, i1 %cond) gc "hotspotgc" {
entry:
  %check1 = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 6 to ptr),
    ptr addrspace(1) nonnull %obj)
  %sel = select i1 %cond, i1 %check1, i1 false
  br i1 %sel, label %is_animal, label %exit

is_animal:
  %check2 = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 6 to ptr),
    ptr addrspace(1) nonnull %obj)
  ret i1 %check2

exit:
  ret i1 false
}

; CHECK-LABEL: @test
; CHECK: is_animal:
; CHECK-NEXT: ret i1 true

!java-method-compilation = !{}
