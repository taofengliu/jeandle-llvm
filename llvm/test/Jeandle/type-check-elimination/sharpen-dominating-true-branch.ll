; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/sharpen-dominating-true-branch.cblog %s 2>&1 | FileCheck %s

; Test: Dominating check passes on true-branch, sharpening type for subsequent
; check to a more specific subtype.
; First check: instanceof klass 6 (Animal). Passes → obj IS Animal.
; Second check: instanceof klass 1 (Object). Since Animal is subtype of Object,
; IsSubtype(6, 1) = true => fold to true.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @test(ptr addrspace(1) nonnull %obj) gc "hotspotgc" {
entry:
  %check1 = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 6 to ptr),
    ptr addrspace(1) nonnull %obj)
  br i1 %check1, label %is_animal, label %exit

is_animal:
  %check2 = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 1 to ptr),
    ptr addrspace(1) nonnull %obj)
  ret i1 %check2

exit:
  ret i1 false
}

; CHECK-LABEL: @test
; CHECK: is_animal:
; CHECK-NEXT: ret i1 true

!java-method-compilation = !{}
