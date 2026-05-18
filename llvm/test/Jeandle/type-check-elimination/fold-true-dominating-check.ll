; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/fold-true-dominating-check.cblog %s 2>&1 | FileCheck %s

; Test: Dominating check_instanceof sharpens type. First check_instanceof for
; klass 6 (Animal) passes on true-branch, then second check for klass 6 is
; redundant since we already know the object is Animal.
; On true branch: obj IS klass 6 (Animal). Second check: instanceof 6 (Animal).
; IsSubtype(6, 6) = true => fold second check to true.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @test(ptr addrspace(1) nonnull %obj) gc "hotspotgc" {
entry:
  %check1 = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 6 to ptr),
    ptr addrspace(1) nonnull %obj)
  br i1 %check1, label %is_animal, label %not_animal

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
