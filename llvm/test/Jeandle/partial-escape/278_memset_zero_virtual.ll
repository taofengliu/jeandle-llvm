; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" -jeandle-vm-callback-log=%S/Inputs/278_memset_zero_virtual.cblog %s | FileCheck %s

; llvm.memset on a virtual int[]. Constant length and constant
; byte value (0xFF) decompose into per-element constants by replicating
; the byte across the element width. For 0xFF/i32 = 0xFFFFFFFF = -1.
; A read of element 1 must observe -1.

declare hotspotcc ptr addrspace(1) @jeandle.newarray(ptr, i32)
declare void @llvm.memset.p1.i64(ptr addrspace(1), i8, i64, i1)
declare i32 @__gxx_personality_v0(...)

define i32 @t() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %arr = invoke hotspotcc ptr addrspace(1) @jeandle.newarray(
            ptr inttoptr (i64 12345 to ptr), i32 4)
         to label %n unwind label %u
n:
  %base = getelementptr inbounds i8, ptr addrspace(1) %arr, i32 16
  ; memset(arr, 0xFF, 16 bytes = 4 i32 elements)
  call void @llvm.memset.p1.i64(ptr addrspace(1) %base, i8 -1, i64 16, i1 false)
  %p1 = getelementptr inbounds i32, ptr addrspace(1) %base, i64 1
  %v = load atomic i32, ptr addrspace(1) %p1 unordered, align 4
  ret i32 %v
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define i32 @t
; CHECK-NOT: jeandle.newarray
; CHECK-NOT: llvm.memset
; CHECK-NOT: load
; CHECK: ret i32 -1

!java-method-compilation = !{}
