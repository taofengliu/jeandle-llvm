; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" -jeandle-vm-callback-log=%S/Inputs/271_arraycopy_memmove_overlap.cblog %s | FileCheck %s

; B9: memmove on the SAME virtual int[] with srcOff=0, dstOff=2, len=3 —
; classic overlap requiring backward semantics. We buffer the source values
; first then write them out, so the per-slot updates remain correct.
;
;   Initial arr: [10, 20, 30, 40, 50]
;   memmove(arr+2, arr+0, 12 bytes / 3 ints)
;   After    arr: [10, 20, 10, 20, 30]
;
; Read arr[4] -> should fold to 30 (the original arr[2], not the overwritten
; one).

declare hotspotcc ptr addrspace(1) @jeandle.newarray(ptr, i32)
declare void @llvm.memmove.p1.p1.i64(ptr addrspace(1), ptr addrspace(1), i64, i1)

declare i32 @__gxx_personality_v0(...)

define i32 @test_memmove_overlap() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %arr = invoke hotspotcc ptr addrspace(1) @jeandle.newarray(
            ptr inttoptr (i64 12345 to ptr), i32 5)
         to label %n unwind label %u
n:
  %base = getelementptr inbounds i8, ptr addrspace(1) %arr, i32 16
  %p0 = getelementptr inbounds i32, ptr addrspace(1) %base, i64 0
  %p1 = getelementptr inbounds i32, ptr addrspace(1) %base, i64 1
  %p2 = getelementptr inbounds i32, ptr addrspace(1) %base, i64 2
  %p3 = getelementptr inbounds i32, ptr addrspace(1) %base, i64 3
  %p4 = getelementptr inbounds i32, ptr addrspace(1) %base, i64 4
  store atomic i32 10, ptr addrspace(1) %p0 unordered, align 4
  store atomic i32 20, ptr addrspace(1) %p1 unordered, align 4
  store atomic i32 30, ptr addrspace(1) %p2 unordered, align 4
  store atomic i32 40, ptr addrspace(1) %p3 unordered, align 4
  store atomic i32 50, ptr addrspace(1) %p4 unordered, align 4
  ; Overlap copy: copy [arr+0..arr+2] into [arr+2..arr+4].
  call void @llvm.memmove.p1.p1.i64(ptr addrspace(1) %p2, ptr addrspace(1) %p0, i64 12, i1 false)
  ; arr[4] should be 30 (the pre-memmove arr[2]).
  %v = load atomic i32, ptr addrspace(1) %p4 unordered, align 4
  ret i32 %v
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define i32 @test_memmove_overlap
; CHECK-NOT: jeandle.newarray
; CHECK-NOT: llvm.memmove
; CHECK-NOT: store
; CHECK-NOT: load
; CHECK: ret i32 30

!java-method-compilation = !{}
