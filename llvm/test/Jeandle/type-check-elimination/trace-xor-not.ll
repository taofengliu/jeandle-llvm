; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/trace-xor-not.cblog %s 2>&1 | FileCheck %s

; Test: `xor i1 %check, true` is logical NOT.
; Negated check: true-branch means check FAILED, false-branch means check PASSED.
; xor true means: when branch is taken (cond=true), the original check was false.
; So on the true-branch of `br i1 %negated`, the check failed → exclusion.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @test(ptr addrspace(1) nonnull %obj) gc "hotspotgc" {
entry:
  %check1 = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 6 to ptr),
    ptr addrspace(1) nonnull %obj)
  %negated = xor i1 %check1, true
  br i1 %negated, label %not_animal, label %is_animal

not_animal:
  %check2 = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 7 to ptr),
    ptr addrspace(1) nonnull %obj)
  ret i1 %check2

is_animal:
  ret i1 true
}

; CHECK-LABEL: @test
; CHECK: not_animal:
; CHECK-NEXT: ret i1 false

!java-method-compilation = !{}
