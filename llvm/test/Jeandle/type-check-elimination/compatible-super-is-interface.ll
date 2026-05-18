; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/compatible-super-is-interface.cblog %s 2>&1 | FileCheck %s

; Test: Super is an interface, obj is non-exact class → cannot prove incompatible.
; Animal (6, non-exact) instanceof Serializable (interface, klass 9):
; areKlassesIncompatible(6, false, 9): IsSubtype(6,9)=false, IsInterface(6)=false,
; !exact → IsSubtype(9,6)=false, IsInterface(9)=true → false.
; A subclass of Animal could implement Serializable.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @test(ptr addrspace(1) nonnull "java-klass"="6" %obj) gc "hotspotgc" {
entry:
  %check = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 9 to ptr),
    ptr addrspace(1) nonnull %obj)
  ret i1 %check
}

; CHECK-LABEL: @test
; CHECK: call i1 @jeandle.check_instanceof
; CHECK: ret i1 %check

!java-method-compilation = !{}
