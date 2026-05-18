; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/type-from-metadata.cblog %s 2>&1 | FileCheck %s

; Test: Object type from !java-klass metadata on a load instruction.
; A load with !java-klass !{i64 7} indicates the loaded object is of type Dog.
; Combined with !java-klass-exact, it means exactly Dog.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @test(ptr addrspace(1) nonnull %field_ptr) gc "hotspotgc" {
entry:
  %obj = load ptr addrspace(1), ptr addrspace(1) %field_ptr, !java-klass !0, !java-klass-exact !1
  %check = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 6 to ptr),
    ptr addrspace(1) nonnull %obj)
  ret i1 %check
}

; Dog (7, exact) instanceof Animal (6) → fold true.
; CHECK-LABEL: @test
; CHECK: ret i1 true

!java-method-compilation = !{}
!0 = !{i64 7}
!1 = !{}
