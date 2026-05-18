; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/type-from-param-attr-exact.cblog %s 2>&1 | FileCheck %s

; Test: Object with exact type from "java-klass"+"java-klass-exact" parameter attributes.
; Function param has exact klass 7 (Dog, final), checking instanceof klass 6 (Animal).
; Dog is NOT a subtype of Cat (klass 8). With exact type, areKlassesIncompatible(7, true, 8).
; IsSubtype(7, 8) = false, IsInterface(7) = false, Exact=true => fold to false.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @test(ptr addrspace(1) nonnull "java-klass"="7" "java-klass-exact" %obj) gc "hotspotgc" {
entry:
  %result = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 8 to ptr),
    ptr addrspace(1) nonnull %obj)
  ret i1 %result
}

; CHECK: ret i1 false

!java-method-compilation = !{}
