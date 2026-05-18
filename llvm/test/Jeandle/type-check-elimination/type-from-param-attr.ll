; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/type-from-param-attr.cblog %s 2>&1 | FileCheck %s

; Test: Object type from "java-klass" parameter attribute.
; Function param has "java-klass"="5" (SubRunnable), checking instanceof klass 4 (MyRunnable).
; IsSubtype(5, 4) = true => fold to true.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @test(ptr addrspace(1) nonnull "java-klass"="5" %obj) gc "hotspotgc" {
entry:
  %result = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 4 to ptr),
    ptr addrspace(1) nonnull %obj)
  ret i1 %result
}

; CHECK: ret i1 true

!java-method-compilation = !{}
