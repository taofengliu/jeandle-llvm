; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/sharpen-icmp-ne-zero.cblog %s 2>&1 | FileCheck %s

; Test: Branch condition is `icmp ne i1 %check, 0` (equivalent to the check itself).
; traceToCheckInstanceof should trace through the icmp and find the check.
; The true-branch (ne 0 = check was true) sharpens to the check's klass.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @test(ptr addrspace(1) nonnull %obj) gc "hotspotgc" {
entry:
  %check1 = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 6 to ptr),
    ptr addrspace(1) nonnull %obj)
  %cond = icmp ne i1 %check1, 0
  br i1 %cond, label %is_animal, label %exit

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
