; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" -jeandle-vm-callback-log=%S/Inputs/276_arraycopy_then_load.cblog %s | FileCheck %s

; B9: arraycopy followed by an element load. The load reads the value that
; arraycopy staged into dst's FieldStates, so it folds to the source's stored
; value (200) without any IR for the alloc / memcpy / store / load surviving.

declare hotspotcc ptr addrspace(1) @jeandle.newarray(ptr, i32)
declare void @llvm.memcpy.p1.p1.i64(ptr addrspace(1), ptr addrspace(1), i64, i1)

declare i32 @__gxx_personality_v0(...)

define i32 @test_then_load() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %src = invoke hotspotcc ptr addrspace(1) @jeandle.newarray(
            ptr inttoptr (i64 12345 to ptr), i32 3)
         to label %n1 unwind label %u
n1:
  %dst = invoke hotspotcc ptr addrspace(1) @jeandle.newarray(
            ptr inttoptr (i64 12345 to ptr), i32 3)
         to label %n2 unwind label %u
n2:
  %sbase = getelementptr inbounds i8, ptr addrspace(1) %src, i32 16
  %s1 = getelementptr inbounds i32, ptr addrspace(1) %sbase, i64 1
  store atomic i32 200, ptr addrspace(1) %s1 unordered, align 4
  %dbase = getelementptr inbounds i8, ptr addrspace(1) %dst, i32 16
  call void @llvm.memcpy.p1.p1.i64(ptr addrspace(1) %dbase, ptr addrspace(1) %sbase, i64 12, i1 false)
  %d1 = getelementptr inbounds i32, ptr addrspace(1) %dbase, i64 1
  %v = load atomic i32, ptr addrspace(1) %d1 unordered, align 4
  ret i32 %v
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define i32 @test_then_load
; CHECK-NOT: jeandle.newarray
; CHECK-NOT: llvm.memcpy
; CHECK-NOT: store
; CHECK-NOT: load
; CHECK: ret i32 200

!java-method-compilation = !{}
