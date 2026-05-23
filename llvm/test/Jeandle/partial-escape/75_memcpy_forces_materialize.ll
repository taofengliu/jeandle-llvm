; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; (§8.1.15): memcpy with a virtual INSTANCE destination is not modelled by
; tier-2; it must conservatively force materialization. The memcpy intrinsic
; is a CallInst whose virtual operand triggers materializeAllVirtualOperands.
;
; (B9, 2026-05-22): tier2ArrayCopy now folds memcpy/memmove into per-slot
; FieldStates updates when the destination is a virtual ARRAY with known
; element metadata. This test's destination is a virtual instance
; (jeandle.new_instance), which does not satisfy the array prerequisite,
; so tier2ArrayCopy bails and the instance materializes here — matching
; the original behavior. See tests 270-277 for the new array-fold cases.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @llvm.memcpy.p1.p0.i64(ptr addrspace(1), ptr, i64, i1)
declare i32 @__gxx_personality_v0(...)

define void @test_memcpy(ptr %src) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %n unwind label %u
n:
  call void @llvm.memcpy.p1.p0.i64(ptr addrspace(1) %o, ptr %src, i64 8, i1 false)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; The allocation survives (in original or materialized form). The memcpy is
; preserved.
; CHECK-LABEL: define void @test_memcpy
; CHECK: jeandle.new_instance
; CHECK: call void @llvm.memcpy

!java-method-compilation = !{}
