; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" -jeandle-vm-callback-log=%S/Inputs/275_arraycopy_partial.cblog %s | FileCheck %s

; Copy only a portion of the array (srcOff=1, dstOff=2, len=2 ints).
; Per-slot copy of the chosen window; dst's untouched slots keep their
; default-zero (or independently-stored) values.
;
;   src = [11, 22, 33, 44]
;   dst[0] is never written -> default 0
;   dst[1] is never written -> default 0
;   dst[2..3] <- src[1..2]   = [22, 33]
;
; Load dst[3] -> 33; load dst[0] -> 0. We assert ret == 33.

declare hotspotcc ptr addrspace(1) @jeandle.newarray(ptr, i32)
declare void @llvm.memcpy.p1.p1.i64(ptr addrspace(1), ptr addrspace(1), i64, i1)

declare i32 @__gxx_personality_v0(...)

define i32 @test_partial() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
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
  %s0 = getelementptr inbounds i32, ptr addrspace(1) %sbase, i64 0
  %s1 = getelementptr inbounds i32, ptr addrspace(1) %sbase, i64 1
  %s2 = getelementptr inbounds i32, ptr addrspace(1) %sbase, i64 2
  %s3 = getelementptr inbounds i32, ptr addrspace(1) %sbase, i64 3
  store atomic i32 11, ptr addrspace(1) %s0 unordered, align 4
  store atomic i32 22, ptr addrspace(1) %s1 unordered, align 4
  store atomic i32 33, ptr addrspace(1) %s2 unordered, align 4
  store atomic i32 44, ptr addrspace(1) %s3 unordered, align 4
  ; dst+2 starts at byte offset 16+2*4=24; src+1 starts at byte 20; 8 bytes / 2 ints.
  %dbase = getelementptr inbounds i8, ptr addrspace(1) %dst, i32 16
  %d2 = getelementptr inbounds i32, ptr addrspace(1) %dbase, i64 2
  call void @llvm.memcpy.p1.p1.i64(ptr addrspace(1) %d2, ptr addrspace(1) %s1, i64 8, i1 false)
  %d3 = getelementptr inbounds i32, ptr addrspace(1) %dbase, i64 3
  %v = load atomic i32, ptr addrspace(1) %d3 unordered, align 4
  ret i32 %v
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define i32 @test_partial
; CHECK-NOT: jeandle.newarray
; CHECK-NOT: llvm.memcpy
; CHECK-NOT: store
; CHECK-NOT: load
; CHECK: ret i32 33

!java-method-compilation = !{}
