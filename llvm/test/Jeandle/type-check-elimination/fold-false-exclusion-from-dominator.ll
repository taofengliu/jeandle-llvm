; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/fold-false-exclusion-from-dominator.cblog %s 2>&1 | FileCheck %s

; Test: Multiple dominating failed checks create multiple exclusions.
; Check 1 for klass 6 (Animal) fails => excluded.
; Check 2 for klass 10 (AbstractList) fails => excluded.
; Now checking instanceof klass 11 (ArrayList extends AbstractList).
; IsSubtype(11, 10) = true => SuperKlass is subtype of excluded => fold to false.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @test(ptr addrspace(1) nonnull %obj) gc "hotspotgc" {
entry:
  %check1 = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 6 to ptr),
    ptr addrspace(1) nonnull %obj)
  br i1 %check1, label %exit_true, label %not_animal

not_animal:
  %check2 = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 10 to ptr),
    ptr addrspace(1) nonnull %obj)
  br i1 %check2, label %exit_true, label %not_list

not_list:
  %check3 = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 11 to ptr),
    ptr addrspace(1) nonnull %obj)
  ret i1 %check3

exit_true:
  ret i1 true
}

; CHECK-LABEL: @test
; CHECK: not_list:
; CHECK-NEXT: ret i1 false

!java-method-compilation = !{}
