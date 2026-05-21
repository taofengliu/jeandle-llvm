; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; PEA: two synchronized regions on two different virtuals, properly
; nested. The outer object escapes via a sink call inside both locks. With
; strict-lock-order assumed, the inner virtual must be cascaded into
; materialization at the same insertion point so the runtime monitor stack
; ordering is preserved. Both objects' monitorenter and monitorexit calls
; are retained on their respective materialized pointers.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc i1 @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1), ptr)
declare hotspotcc i1 @jeandle.monitorexit_with_lightweight_lock(ptr addrspace(1), ptr)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_strict_lock_cascade() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %lock_a = alloca i64, align 8
  %lock_b = alloca i64, align 8
  %a = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %na unwind label %u
na:
  %b = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 67890 to ptr), i32 16)
       to label %nb unwind label %u
nb:
  ; Acquire outer A.
  %ea = call hotspotcc i1 @jeandle.monitorenter_with_lightweight_lock(
                  ptr addrspace(1) %a, ptr %lock_a)
  ; Acquire inner B.
  %eb = call hotspotcc i1 @jeandle.monitorenter_with_lightweight_lock(
                  ptr addrspace(1) %b, ptr %lock_b)
  ; Escape: A leaks through a sink while both locks held.
  call void @sink(ptr addrspace(1) %a)
  ; Release inner B.
  %xb = call hotspotcc i1 @jeandle.monitorexit_with_lightweight_lock(
                  ptr addrspace(1) %b, ptr %lock_b)
  ; Release outer A.
  %xa = call hotspotcc i1 @jeandle.monitorexit_with_lightweight_lock(
                  ptr addrspace(1) %a, ptr %lock_a)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; Both A and B must be materialized.
; CHECK-LABEL: define void @test_strict_lock_cascade
; CHECK-DAG: %[[MATA:[A-Za-z0-9._]+]] = invoke hotspotcc{{.*}}ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 12345 to ptr), i32 16)
; CHECK-DAG: %[[MATB:[A-Za-z0-9._]+]] = invoke hotspotcc{{.*}}ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 67890 to ptr), i32 16)
; A's enter on materialized A.
; CHECK: call hotspotcc i1 @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1) %[[MATA]],
; B's enter on materialized B.
; CHECK: call hotspotcc i1 @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1) %[[MATB]],
; Sink on materialized A.
; CHECK: call void @sink(ptr addrspace(1) %[[MATA]])
; B's exit on materialized B.
; CHECK: call hotspotcc i1 @jeandle.monitorexit_with_lightweight_lock(ptr addrspace(1) %[[MATB]],
; A's exit on materialized A.
; CHECK: call hotspotcc i1 @jeandle.monitorexit_with_lightweight_lock(ptr addrspace(1) %[[MATA]],

!java-method-compilation = !{}
