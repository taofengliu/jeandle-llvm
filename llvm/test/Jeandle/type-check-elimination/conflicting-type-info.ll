; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/conflicting-type-info.cblog %s 2>&1 | FileCheck %s

; Test: typeIntersect with contradictory positive types.
; Base type from param attr: Animal (6, non-exact).
; Dominating check: instanceof Dog (7) passes → sharpens to Dog (7, final → exact).
; typeIntersect({6, false}, {7, true}): IsSubtype(6,7)=false, IsSubtype(7,6)=true
; → picks B (Dog, exact). Final type: {7, exact}.
; Then: Dog (7, exact) instanceof Cat (8) → fold false (exact, incompatible).
;
; This is NOT contradictory — Dog IS-A Animal, so the info is consistent.
; (True contradictions can't happen without dead code, tested indirectly.)

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @test(ptr addrspace(1) nonnull "java-klass"="6" %obj) gc "hotspotgc" {
entry:
  %is_dog = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 7 to ptr),
    ptr addrspace(1) nonnull %obj)
  br i1 %is_dog, label %dog_true, label %exit

dog_true:
  ; typeIntersect(param={6,false}, sharpened={7,exact}) → {7, exact}.
  ; Dog (7, exact) vs Cat (8): incompatible → false.
  %check = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 8 to ptr),
    ptr addrspace(1) nonnull %obj)
  ret i1 %check

exit:
  ret i1 false
}

; CHECK-LABEL: @test
; CHECK: dog_true:
; CHECK-NEXT: ret i1 false

!java-method-compilation = !{}
