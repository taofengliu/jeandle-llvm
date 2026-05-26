; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" -jeandle-vm-callback-log=%S/Inputs/270_arraycopy_virt_to_virt_int.cblog %s | FileCheck %s

; Virtual int[] -> virtual int[] arraycopy via llvm.memcpy. Both
; allocations and the memcpy are eliminated; the loaded value sees the
; element copied from src into dst.

declare hotspotcc ptr addrspace(1) @jeandle.newarray(ptr, i32)
declare void @llvm.memcpy.p1.p1.i64(ptr addrspace(1), ptr addrspace(1), i64, i1)

declare i32 @__gxx_personality_v0(...)

define i32 @test_arraycopy_int() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %src = invoke hotspotcc ptr addrspace(1) @jeandle.newarray(
            ptr inttoptr (i64 12345 to ptr), i32 4)
         to label %n1 unwind label %u
n1:
  %dst = invoke hotspotcc ptr addrspace(1) @jeandle.newarray(
            ptr inttoptr (i64 12345 to ptr), i32 4)
         to label %n2 unwind label %u
n2:
  ; Populate src[0..3] = {10, 20, 30, 40}.
  %sbase = getelementptr inbounds i8, ptr addrspace(1) %src, i32 16
  %s0 = getelementptr inbounds i32, ptr addrspace(1) %sbase, i64 0
  %s1 = getelementptr inbounds i32, ptr addrspace(1) %sbase, i64 1
  %s2 = getelementptr inbounds i32, ptr addrspace(1) %sbase, i64 2
  %s3 = getelementptr inbounds i32, ptr addrspace(1) %sbase, i64 3
  store atomic i32 10, ptr addrspace(1) %s0 unordered, align 4
  store atomic i32 20, ptr addrspace(1) %s1 unordered, align 4
  store atomic i32 30, ptr addrspace(1) %s2 unordered, align 4
  store atomic i32 40, ptr addrspace(1) %s3 unordered, align 4
  ; arraycopy(src, 0, dst, 0, 4): memcpy 16 bytes from src+16 to dst+16.
  %dbase = getelementptr inbounds i8, ptr addrspace(1) %dst, i32 16
  call void @llvm.memcpy.p1.p1.i64(ptr addrspace(1) %dbase, ptr addrspace(1) %sbase, i64 16, i1 false)
  ; Read dst[2] — should fold to 30.
  %d2 = getelementptr inbounds i32, ptr addrspace(1) %dbase, i64 2
  %v = load atomic i32, ptr addrspace(1) %d2 unordered, align 4
  ret i32 %v
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define i32 @test_arraycopy_int
; CHECK-NOT: jeandle.newarray
; CHECK-NOT: llvm.memcpy
; CHECK-NOT: store
; CHECK-NOT: load
; CHECK: ret i32 30

!java-method-compilation = !{}
