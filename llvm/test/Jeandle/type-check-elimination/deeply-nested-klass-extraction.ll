; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/deeply-nested-klass-extraction.cblog %s 2>&1 | FileCheck %s

; Test: Klass constant encoded through a chain of freezes exceeding
; MaxExtractKlassDepth (16). extractKlassConstant gives up and returns 0.
; The check is preserved.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @test(ptr addrspace(1) nonnull "java-klass"="7" "java-klass-exact" %obj) gc "hotspotgc" {
entry:
  ; 17 levels of freeze wrapping (exceeds MaxExtractKlassDepth=16).
  %f1 = freeze ptr inttoptr (i64 6 to ptr)
  %f2 = freeze ptr %f1
  %f3 = freeze ptr %f2
  %f4 = freeze ptr %f3
  %f5 = freeze ptr %f4
  %f6 = freeze ptr %f5
  %f7 = freeze ptr %f6
  %f8 = freeze ptr %f7
  %f9 = freeze ptr %f8
  %f10 = freeze ptr %f9
  %f11 = freeze ptr %f10
  %f12 = freeze ptr %f11
  %f13 = freeze ptr %f12
  %f14 = freeze ptr %f13
  %f15 = freeze ptr %f14
  %f16 = freeze ptr %f15
  %f17 = freeze ptr %f16
  %check = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) %f17,
    ptr addrspace(1) nonnull %obj)
  ret i1 %check
}

; extractKlassConstant returns 0 → check is preserved.
; CHECK-LABEL: @test
; CHECK: call i1 @jeandle.check_instanceof
; CHECK: ret i1 %check

!java-method-compilation = !{}
