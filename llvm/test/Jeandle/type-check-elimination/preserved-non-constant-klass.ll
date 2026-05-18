; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/preserved-non-constant-klass.cblog %s 2>&1 | FileCheck %s

; Test: Non-constant klass argument (loaded at runtime) → extractKlassConstant
; returns 0, check is preserved.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

@runtime_klass = external global ptr

define i1 @test(ptr addrspace(1) nonnull %obj) gc "hotspotgc" {
entry:
  %klass = load ptr, ptr @runtime_klass
  %check = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) %klass,
    ptr addrspace(1) nonnull %obj)
  ret i1 %check
}

; CHECK-LABEL: @test
; CHECK: call i1 @jeandle.check_instanceof
; CHECK: ret i1 %check

!java-method-compilation = !{}
