; RUN: opt -S -passes="type-check-elimination" %s 2>&1 | FileCheck %s

; Test: Module without !java-method-compilation metadata → pass is a no-op.
; No .cblog needed because no VM callbacks are made.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @test(ptr addrspace(1) nonnull %obj) gc "hotspotgc" {
entry:
  %check = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 6 to ptr),
    ptr addrspace(1) nonnull %obj)
  ret i1 %check
}

; CHECK-LABEL: @test
; CHECK: call i1 @jeandle.check_instanceof
; CHECK: ret i1 %check
