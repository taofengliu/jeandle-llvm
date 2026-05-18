; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/klass-global-load.cblog %s 2>&1 | FileCheck %s

; Test: SuperKlass obtained by loading from a constant global variable.
; extractKlassConstant should recurse into the global's initializer.
; @klass_global = constant i64 4, so loading yields klass 4.
; Hierarchy: obj has klass 5 (SubRunnable), checking instanceof klass 4 (MyRunnable).
; IsSubtype(5, 4) = true => fold to true.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

@klass_global = constant i64 4
@glob = external addrspace(1) global ptr addrspace(1)

define i1 @test() gc "hotspotgc" {
entry:
  %obj = load ptr addrspace(1), ptr addrspace(1) @glob, !java-klass !0
  %klass_ptr = load ptr, ptr @klass_global
  %result = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) %klass_ptr,
    ptr addrspace(1) nonnull %obj)
  ret i1 %result
}

!0 = !{i64 5}

; CHECK: ret i1 true

!java-method-compilation = !{}
