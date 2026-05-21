; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; PEA: alloc in entry, store one field in normal, return the oop.
; The return is an escape point — PEA materializes the allocation as an
; InvokeInst right before the ret, splitting the block so the new invoke is
; the terminator, replays the field store on the normal-dest, and erases the
; original allocation invoke. The original allocation site disappears; the
; materialized invoke takes its place and reuses the original unwind dest.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare i32 @__gxx_personality_v0(...)

define ptr addrspace(1) @test_mat_return(i32 %x) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %n unwind label %u
n:
  %s = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  store atomic i32 %x, ptr addrspace(1) %s unordered, align 4
  ret ptr addrspace(1) %o
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define ptr addrspace(1) @test_mat_return
; CHECK: %[[MAT:[A-Za-z0-9._]+]] = invoke hotspotcc{{.*}}ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 12345 to ptr), i32 16)
; CHECK-NEXT: to label %{{.*}} unwind label %{{.*}}
; CHECK: %[[SLOT:[A-Za-z0-9._]+]] = getelementptr inbounds i8, ptr addrspace(1) %[[MAT]], i64 8
; CHECK: store atomic i32 %x, ptr addrspace(1) %[[SLOT]] unordered, align {{[0-9]+}}
; CHECK: ret ptr addrspace(1) %[[MAT]]

!java-method-compilation = !{}
