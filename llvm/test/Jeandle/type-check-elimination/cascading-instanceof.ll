; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/cascading-instanceof.cblog %s 2>&1 | FileCheck %s

; Test: Cascading if-else instanceof checks on the same object.
; Pattern: if (x instanceof Dog) ... else if (x instanceof Cat) ...
; After the first check fails (false branch), the second check can use
; the exclusion information (Dog is excluded on the false branch).
; Dog (7, extends Animal 6, final). Cat (8, extends Animal 6).
; On false branch of Dog check: exclusion {7}. Cat(8) is NOT subtype of 7,
; so the Cat check is preserved.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i32 @dispatch(ptr addrspace(1) nonnull "java-klass"="6" %obj) gc "hotspotgc" {
entry:
  %is_dog = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 7 to ptr),
    ptr addrspace(1) nonnull %obj)
  br i1 %is_dog, label %dog_case, label %not_dog

not_dog:
  %is_cat = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 8 to ptr),
    ptr addrspace(1) nonnull %obj)
  br i1 %is_cat, label %cat_case, label %other

dog_case:
  ret i32 1

cat_case:
  ret i32 2

other:
  ret i32 0
}

; The Dog check is preserved (Animal is non-exact, Dog is a valid subtype).
; The Cat check is also preserved (Cat is not subtype of excluded Dog).
; CHECK-LABEL: @dispatch
; CHECK: %is_dog = call i1 @jeandle.check_instanceof
; CHECK: %is_cat = call i1 @jeandle.check_instanceof

!java-method-compilation = !{}
