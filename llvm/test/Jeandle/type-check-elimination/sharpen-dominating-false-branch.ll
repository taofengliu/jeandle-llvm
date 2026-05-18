; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/sharpen-dominating-false-branch.cblog %s 2>&1 | FileCheck %s

; Test: Dominating check fails on false-branch, adding exclusion that enables
; folding a subsequent check to false.
; First check: instanceof klass 6 (Animal). Fails → obj IS NOT Animal.
; Second check: instanceof klass 7 (Dog extends Animal).
; Since Dog(7) is subtype of excluded Animal(6), fold to false.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @test(ptr addrspace(1) nonnull %obj) gc "hotspotgc" {
entry:
  %check1 = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 6 to ptr),
    ptr addrspace(1) nonnull %obj)
  br i1 %check1, label %exit_true, label %not_animal

not_animal:
  %check2 = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 7 to ptr),
    ptr addrspace(1) nonnull %obj)
  ret i1 %check2

exit_true:
  ret i1 true
}

; CHECK-LABEL: @test
; CHECK: not_animal:
; CHECK-NEXT: ret i1 false

!java-method-compilation = !{}
