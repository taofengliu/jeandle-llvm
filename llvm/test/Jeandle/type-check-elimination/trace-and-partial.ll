; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/trace-and-partial.cblog %s 2>&1 | FileCheck %s

; Test: `and i1 %checkA, %unrelated` where only one side traces to a check.
; True-branch: And being true means both are true, so the matched operand is true.
; The matched check's TrueKlass applies on the true-branch.
; False-branch: And being false could be due to the unmatched operand → unsound.
; Only true-branch constraint is valid.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)
declare i1 @some_condition()

define i1 @test(ptr addrspace(1) nonnull %obj) gc "hotspotgc" {
entry:
  %checkA = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 6 to ptr),
    ptr addrspace(1) nonnull %obj)
  %other = call i1 @some_condition()
  %both = and i1 %checkA, %other
  br i1 %both, label %is_animal, label %exit

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
