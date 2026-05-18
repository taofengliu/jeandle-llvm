; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/incompatible-non-exact-class-vs-class.cblog %s 2>&1 | FileCheck %s

; Test: Non-exact class incompatible with unrelated class.
; Animal (6, non-exact) vs String (2): neither is subtype of the other,
; neither is an interface → incompatible → fold to false.
; This works because Java has single-class inheritance: if A !sub B and B !sub A
; and neither is interface, then no subclass of A can be a B.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @test(ptr addrspace(1) nonnull "java-klass"="6" %obj) gc "hotspotgc" {
entry:
  %check = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 2 to ptr),
    ptr addrspace(1) nonnull %obj)
  ret i1 %check
}

; CHECK-LABEL: @test
; CHECK: ret i1 false

!java-method-compilation = !{}
