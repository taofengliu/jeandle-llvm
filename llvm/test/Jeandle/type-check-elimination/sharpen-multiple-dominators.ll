; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/sharpen-multiple-dominators.cblog %s 2>&1 | FileCheck %s

; Test: Multiple dominating checks combine via type intersection.
; Check 1 passes for klass 6 (Animal) → obj IS Animal.
; Check 2 passes for klass 7 (Dog extends Animal) → obj IS Dog.
; Third check: instanceof klass 6 (Animal). Since Dog(7) is subtype of Animal(6),
; IsSubtype(7, 6) = true => fold to true.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @test(ptr addrspace(1) nonnull %obj) gc "hotspotgc" {
entry:
  %check1 = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 6 to ptr),
    ptr addrspace(1) nonnull %obj)
  br i1 %check1, label %is_animal, label %exit

is_animal:
  %check2 = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 7 to ptr),
    ptr addrspace(1) nonnull %obj)
  br i1 %check2, label %is_dog, label %exit

is_dog:
  %check3 = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 6 to ptr),
    ptr addrspace(1) nonnull %obj)
  ret i1 %check3

exit:
  ret i1 false
}

; CHECK-LABEL: @test
; CHECK: is_dog:
; CHECK-NEXT: ret i1 true

!java-method-compilation = !{}
