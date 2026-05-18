; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/type-from-return-attr.cblog %s 2>&1 | FileCheck %s

; Test: Object type from "java-klass" return attribute on a call.
; The call returns an object with klass 5 (SubRunnable),
; then we check instanceof klass 4 (MyRunnable).
; IsSubtype(5, 4) = true => fold to true.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

declare "java-klass"="5" ptr addrspace(1) @get_object()

define i1 @test() gc "hotspotgc" {
entry:
  %obj = call "java-klass"="5" ptr addrspace(1) @get_object()
  %result = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 4 to ptr),
    ptr addrspace(1) nonnull %obj)
  ret i1 %result
}

; CHECK: ret i1 true

!java-method-compilation = !{}
