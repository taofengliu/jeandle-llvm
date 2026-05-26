; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" -jeandle-vm-callback-log=%S/Inputs/273_arraycopy_symbolic_length_bails.cblog %s | FileCheck %s

; Memcpy with a symbolic (function-arg) length. Graal's
; BasicArrayCopyNode.virtualize requires `replacedLength.isConstant()`; we
; mirror that — symbolic length flips the dst VO ineligible. dst's alloc and
; the memcpy must survive in IR (src is virtual too and also flipped
; ineligible so its alloc + stores survive).

declare hotspotcc ptr addrspace(1) @jeandle.newarray(ptr, i32)
declare void @llvm.memcpy.p1.p1.i64(ptr addrspace(1), ptr addrspace(1), i64, i1)

declare i32 @__gxx_personality_v0(...)

define void @test_symbolic_len(i64 %len) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %src = invoke hotspotcc ptr addrspace(1) @jeandle.newarray(
            ptr inttoptr (i64 12345 to ptr), i32 4)
         to label %n1 unwind label %u
n1:
  %dst = invoke hotspotcc ptr addrspace(1) @jeandle.newarray(
            ptr inttoptr (i64 12345 to ptr), i32 4)
         to label %n2 unwind label %u
n2:
  %sbase = getelementptr inbounds i8, ptr addrspace(1) %src, i32 16
  %dbase = getelementptr inbounds i8, ptr addrspace(1) %dst, i32 16
  call void @llvm.memcpy.p1.p1.i64(ptr addrspace(1) %dbase, ptr addrspace(1) %sbase, i64 %len, i1 false)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; Both newarray allocations survive (eligible flipped to false on the bail);
; the memcpy itself stays in IR.
; CHECK-LABEL: define void @test_symbolic_len
; CHECK: jeandle.newarray
; CHECK: jeandle.newarray
; CHECK: call void @llvm.memcpy

!java-method-compilation = !{}
