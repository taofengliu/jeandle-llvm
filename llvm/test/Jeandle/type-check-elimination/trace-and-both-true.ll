; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/trace-and-both-true.cblog %s 2>&1 | FileCheck %s

; Test: `and i1 %checkA, %checkB` as branch condition.
; True-branch: both checks passed (AllOf semantics).
; checkA: instanceof 6 (Animal), checkB: instanceof 7 (Dog extends Animal).
; True-branch sharpens to Dog (more specific). Third check for Animal folds.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @test(ptr addrspace(1) nonnull %obj) gc "hotspotgc" {
entry:
  %checkA = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 6 to ptr),
    ptr addrspace(1) nonnull %obj)
  %checkB = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 7 to ptr),
    ptr addrspace(1) nonnull %obj)
  %both = and i1 %checkA, %checkB
  br i1 %both, label %is_both, label %exit

is_both:
  %check3 = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 6 to ptr),
    ptr addrspace(1) nonnull %obj)
  ret i1 %check3

exit:
  ret i1 false
}

; CHECK-LABEL: @test
; CHECK: is_both:
; CHECK-NEXT: ret i1 true

!java-method-compilation = !{}
