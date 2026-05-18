; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/effectively-final-sharpening.cblog %s 2>&1 | FileCheck %s

; Test: Dominating check passes → sharpenFromDominators calls IsEffectivelyFinal.
; If the sharpened klass IS effectively final, subsequent checks see Exact=true.
; This enables areKlassesIncompatible to fold more aggressively.
;
; Flow: check_instanceof(Dog=7, obj) passes → on true branch, obj type is
; sharpened to Dog(7). IsEffectivelyFinal(7)=true → Exact=true.
; Then check_instanceof(Cat=8, obj): exact Dog vs Cat → incompatible → false.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @test(ptr addrspace(1) nonnull %obj) gc "hotspotgc" {
entry:
  %is_dog = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 7 to ptr),
    ptr addrspace(1) nonnull %obj)
  br i1 %is_dog, label %is_dog_true, label %exit

is_dog_true:
  %is_cat = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 8 to ptr),
    ptr addrspace(1) nonnull %obj)
  ret i1 %is_cat

exit:
  ret i1 false
}

; CHECK-LABEL: @test
; CHECK: is_dog_true:
; CHECK-NEXT: ret i1 false

!java-method-compilation = !{}
