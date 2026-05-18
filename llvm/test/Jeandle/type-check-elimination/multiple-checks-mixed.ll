; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/multiple-checks-mixed.cblog %s 2>&1 | FileCheck %s

; Test: Multiple checks in one function — some fold true, some fold false, some preserved.
; Verifies the pass correctly handles each independently.
; obj has type Animal (klass 6, non-exact).

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define void @test(ptr addrspace(1) nonnull "java-klass"="6" %obj) gc "hotspotgc" {
entry:
  ; check1: Animal (6) instanceof Object (1) → fold true (IsObjectKlass)
  %check1 = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 1 to ptr),
    ptr addrspace(1) nonnull %obj)

  ; check2: Animal (6) instanceof Animal (6) → fold true (same klass, IsSubtype(6,6)=true)
  %check2 = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 6 to ptr),
    ptr addrspace(1) nonnull %obj)

  ; check3: Animal (6, non-exact) instanceof Cat (8) → fold false (incompatible non-exact)
  ; areKlassesIncompatible(6, false, 8): IsSubtype(6,8)=false, IsInterface(6)=false,
  ; !exact so check: IsSubtype(8,6)=true → false! NOT incompatible. Cat extends Animal.
  ; Actually Cat(8) extends Animal(6), so IsSubtype(8,6)=true means "Cat is subtype of Animal".
  ; areKlassesIncompatible would return false because the object COULD be Cat at runtime.
  ; Let's use String (klass 2, final) instead — String doesn't extend Animal.
  %check3 = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 2 to ptr),
    ptr addrspace(1) nonnull %obj)

  ; check4: Animal (6, non-exact) instanceof Runnable (3, interface) → preserved
  ; areKlassesIncompatible(6, false, 3): IsSubtype(6,3)=false, IsInterface(6)=false,
  ; !exact → IsSubtype(3,6)=false, IsInterface(3)=true → not incompatible
  %check4 = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 3 to ptr),
    ptr addrspace(1) nonnull %obj)

  call void @use(i1 %check1, i1 %check2, i1 %check3, i1 %check4)
  ret void
}

declare void @use(i1, i1, i1, i1)

; CHECK-LABEL: @test
; CHECK: %check4 = call i1 @jeandle.check_instanceof(ptr inttoptr (i64 3 to ptr), ptr addrspace(1) nonnull %obj)
; CHECK-NEXT: call void @use(i1 true, i1 true, i1 false, i1 %check4)
; CHECK-NEXT: ret void

!java-method-compilation = !{}
