; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/incompatible-exact-class-vs-class.cblog %s 2>&1 | FileCheck %s

; Test: Exact class type incompatible with unrelated class.
; Dog (7, exact) vs Cat (8): Dog is not a subtype of Cat, not an interface,
; and is exact → incompatible → fold to false.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @test(ptr addrspace(1) nonnull "java-klass"="7" "java-klass-exact" %obj) gc "hotspotgc" {
entry:
  %check = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 8 to ptr),
    ptr addrspace(1) nonnull %obj)
  ret i1 %check
}

; CHECK-LABEL: @test
; CHECK: ret i1 false

!java-method-compilation = !{}
