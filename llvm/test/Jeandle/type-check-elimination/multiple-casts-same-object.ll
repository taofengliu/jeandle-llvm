; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/multiple-casts-same-object.cblog %s 2>&1 | FileCheck %s

; Test: Same object checked for multiple types in sequence, each narrows the
; type further. Pattern: if (obj instanceof Animal) { if (obj instanceof Dog) ... }
; First check passes (Animal, klass 6). Second check (Dog, klass 7) uses the
; sharpened type from the first check. Since Dog extends Animal and the first
; check only gives non-exact Animal, the Dog check is preserved.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @test(ptr addrspace(1) nonnull %obj) gc "hotspotgc" {
entry:
  %is_animal = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 6 to ptr),
    ptr addrspace(1) nonnull %obj)
  br i1 %is_animal, label %check_dog, label %exit

check_dog:
  ; obj is known to be Animal (non-exact unless effectively final).
  ; Check if it's specifically a Dog.
  %is_dog = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 7 to ptr),
    ptr addrspace(1) nonnull %obj)
  ret i1 %is_dog

exit:
  ret i1 false
}

; Animal is NOT effectively final, so the sharpened type is {6, non-exact}.
; Dog(7) is a subtype of Animal(6), so the Dog check cannot be eliminated.
; CHECK-LABEL: @test
; CHECK: check_dog:
; CHECK-NEXT: %is_dog = call i1 @jeandle.check_instanceof

!java-method-compilation = !{}
