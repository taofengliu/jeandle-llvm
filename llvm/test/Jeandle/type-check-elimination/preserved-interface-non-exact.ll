; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/preserved-interface-non-exact.cblog %s 2>&1 | FileCheck %s

; Test: Object is non-exact non-interface class, super is interface.
; areKlassesIncompatible returns false because super is an interface.
; Any non-final class could implement any interface at runtime.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @test(ptr addrspace(1) nonnull "java-klass"="6" %obj) gc "hotspotgc" {
entry:
  ; Check: is Animal (klass 6) an instance of Runnable (interface, klass 3)?
  %check = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 3 to ptr),
    ptr addrspace(1) nonnull %obj)
  ret i1 %check
}

; CHECK-LABEL: @test
; CHECK: call i1 @jeandle.check_instanceof
; CHECK: ret i1 %check

!java-method-compilation = !{}
