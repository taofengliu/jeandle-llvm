; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/instanceof-preserved.cblog %s 2>&1 | FileCheck %s

; Test: object with unknown type (no java-klass metadata) cannot be eliminated.
; The check is preserved as-is.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @test(ptr addrspace(1) nonnull %obj) gc "hotspotgc" {
entry:
  %result = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 4 to ptr),
    ptr addrspace(1) nonnull %obj)
  ret i1 %result
}

; CHECK: call i1 @jeandle.check_instanceof

!java-method-compilation = !{}
