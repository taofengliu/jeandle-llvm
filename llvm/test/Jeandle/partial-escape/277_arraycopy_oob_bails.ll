; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" -jeandle-vm-callback-log=%S/Inputs/277_arraycopy_oob_bails.cblog %s | FileCheck %s

; dstOff + count > dst.ArrayLength must bail (checkBounds).
; dst.length=2, dstOff=1, count=2 -> needs slots 1 and 2 but dst only
; has slots 0..1. Bail; both allocations are materialized and the
; memcpy stays.

declare hotspotcc ptr addrspace(1) @jeandle.newarray(ptr, i32)
declare void @llvm.memcpy.p1.p1.i64(ptr addrspace(1), ptr addrspace(1), i64, i1)

declare i32 @__gxx_personality_v0(...)

define void @test_oob() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %src = invoke hotspotcc ptr addrspace(1) @jeandle.newarray(
            ptr inttoptr (i64 12345 to ptr), i32 4)
         to label %n1 unwind label %u
n1:
  %dst = invoke hotspotcc ptr addrspace(1) @jeandle.newarray(
            ptr inttoptr (i64 12345 to ptr), i32 2)
         to label %n2 unwind label %u
n2:
  %sbase = getelementptr inbounds i8, ptr addrspace(1) %src, i32 16
  %dbase = getelementptr inbounds i8, ptr addrspace(1) %dst, i32 16
  %d1 = getelementptr inbounds i32, ptr addrspace(1) %dbase, i64 1
  ; 8 bytes = 2 ints starting at dst[1] reaches dst[2], out-of-bounds.
  call void @llvm.memcpy.p1.p1.i64(ptr addrspace(1) %d1, ptr addrspace(1) %sbase, i64 8, i1 false)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @test_oob
; CHECK: jeandle.newarray
; CHECK: jeandle.newarray
; CHECK: call void @llvm.memcpy

!java-method-compilation = !{}
