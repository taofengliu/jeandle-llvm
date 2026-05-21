; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Edge case: virtual with zero stores between alloc and escape.
; Materialization with zero FieldEntries: the allocator zero-fills, so no
; replay stores are needed. Just the materialization invoke and the use of
; its result.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_empty_alloc_escape() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %n unwind label %u
n:
  call void @sink(ptr addrspace(1) %o)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; A new alloc invoke appears for materialization (no replay stores needed,
; since there are no recorded fields - the allocator zero-fills).
; CHECK-LABEL: define void @test_empty_alloc_escape
; CHECK: %[[MAT:[A-Za-z0-9._]+]] = invoke hotspotcc{{.*}}ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 12345 to ptr), i32 16)
; CHECK-NOT: store atomic
; CHECK: call void @sink(ptr addrspace(1) %[[MAT]])

!java-method-compilation = !{}
