; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/fold-false-exclusion-subtype.cblog %s 2>&1 | FileCheck %s

; Test: Fold to false via ExcludedKlasses. A dominating check for klass 6 (Animal)
; fails, adding klass 6 to ExcludedKlasses. Then checking instanceof klass 7 (Dog).
; Since Dog is a subtype of Animal, and Animal is excluded, Dog is also excluded.
; IsSubtype(7, 6) = true => SuperKlass(7) is subtype of Excluded(6) => fold to false.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @test(ptr addrspace(1) nonnull %obj) gc "hotspotgc" {
entry:
  %check1 = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 6 to ptr),
    ptr addrspace(1) nonnull %obj)
  br i1 %check1, label %is_animal, label %not_animal

is_animal:
  ret i1 true

not_animal:
  %check2 = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 7 to ptr),
    ptr addrspace(1) nonnull %obj)
  ret i1 %check2
}

; CHECK-LABEL: @test
; CHECK: not_animal:
; CHECK-NEXT: ret i1 false

!java-method-compilation = !{}
