; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/sharpen-zext-icmp.cblog %s 2>&1 | FileCheck %s

; Test: The common pattern after jeandle.instanceof inlining:
;   %check = call i1 @jeandle.check_instanceof(...)
;   %ext = zext i1 %check to i32
;   %cond = icmp ne i32 %ext, 0
;   br i1 %cond, ...
; traceToCheckInstanceof should trace through zext and icmp.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @test(ptr addrspace(1) nonnull %obj) gc "hotspotgc" {
entry:
  %check1 = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 6 to ptr),
    ptr addrspace(1) nonnull %obj)
  %ext = zext i1 %check1 to i32
  %cond = icmp ne i32 %ext, 0
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
