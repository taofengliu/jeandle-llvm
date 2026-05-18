; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/sharpen-icmp-eq-zero.cblog %s 2>&1 | FileCheck %s

; Test: Branch condition is `icmp eq i1 %check, 0` (negated check).
; true-branch means check=0 (failed) → exclusion on true-branch.
; false-branch means check=1 (passed) → positive type on false-branch.
; We test the false-branch (check passed case).

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @test(ptr addrspace(1) nonnull %obj) gc "hotspotgc" {
entry:
  %check1 = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 6 to ptr),
    ptr addrspace(1) nonnull %obj)
  %cond = icmp eq i1 %check1, 0
  br i1 %cond, label %not_animal, label %is_animal

is_animal:
  %check2 = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 6 to ptr),
    ptr addrspace(1) nonnull %obj)
  ret i1 %check2

not_animal:
  ret i1 false
}

; CHECK-LABEL: @test
; CHECK: is_animal:
; CHECK-NEXT: ret i1 true

!java-method-compilation = !{}
