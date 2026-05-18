; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/compatible-interface-obj.cblog %s 2>&1 | FileCheck %s

; Test: Object klass is an interface — cannot prove incompatible, preserved.
; Runnable (interface, klass 3, non-exact) vs String (2):
; areKlassesIncompatible(3, false, 2): IsSubtype(3,2)=false, IsInterface(3)=true → false.
; Any implementor of Runnable could in theory also be a String (can't disprove).

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @test(ptr addrspace(1) nonnull "java-klass"="3" %obj) gc "hotspotgc" {
entry:
  %check = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 2 to ptr),
    ptr addrspace(1) nonnull %obj)
  ret i1 %check
}

; CHECK-LABEL: @test
; CHECK: call i1 @jeandle.check_instanceof
; CHECK: ret i1 %check

!java-method-compilation = !{}
