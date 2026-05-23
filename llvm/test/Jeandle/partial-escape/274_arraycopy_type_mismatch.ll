; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" -jeandle-vm-callback-log=%S/Inputs/274_arraycopy_type_mismatch.cblog %s | FileCheck %s

; B9: memcpy where src is a virtual byte[] (scale 1) and dst is a virtual
; int[] (scale 4). The element type / scale mismatch fails Graal's component-
; type check (BasicArrayCopyNode.java:311) — bail. Both allocations are
; materialized at the memcpy and the memcpy survives.

declare hotspotcc ptr addrspace(1) @jeandle.newarray(ptr, i32)
declare void @llvm.memcpy.p1.p1.i64(ptr addrspace(1), ptr addrspace(1), i64, i1)

declare i32 @__gxx_personality_v0(...)

define void @test_type_mismatch() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %src = invoke hotspotcc ptr addrspace(1) @jeandle.newarray(
            ptr inttoptr (i64 11111 to ptr), i32 16)
         to label %n1 unwind label %u
n1:
  %dst = invoke hotspotcc ptr addrspace(1) @jeandle.newarray(
            ptr inttoptr (i64 12345 to ptr), i32 4)
         to label %n2 unwind label %u
n2:
  %sbase = getelementptr inbounds i8, ptr addrspace(1) %src, i32 16
  %dbase = getelementptr inbounds i8, ptr addrspace(1) %dst, i32 16
  call void @llvm.memcpy.p1.p1.i64(ptr addrspace(1) %dbase, ptr addrspace(1) %sbase, i64 16, i1 false)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; Both newarray sites survive (materialized) and the memcpy stays.
; CHECK-LABEL: define void @test_type_mismatch
; CHECK: jeandle.newarray
; CHECK: jeandle.newarray
; CHECK: call void @llvm.memcpy

!java-method-compilation = !{}
