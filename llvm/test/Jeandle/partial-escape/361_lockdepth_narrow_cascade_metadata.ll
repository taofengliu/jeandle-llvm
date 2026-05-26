; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Narrow cascade keyed by !jeandle.lock_depth metadata. Mirrors
; partial-escape/190_narrow_lock_cascade_nested.ll but with explicit depth
; metadata on each monitorenter. Outer A (depth=0), inner B (depth=1).
; Inner B escapes; the cascade rule
;   other.front().BytecodeDepth < this.back().BytecodeDepth
; selects A (A.minDepth=0 < B.maxDepth=1), so A also materialises.
;
; The behaviour is identical to test 190 — the point here is to drive the
; depth-aware rule end-to-end with metadata present.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc i1 @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1), ptr)
declare hotspotcc i1 @jeandle.monitorexit_with_lightweight_lock(ptr addrspace(1), ptr)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_lockdepth_cascade_metadata() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %lock_a = alloca i64, align 8
  %lock_b = alloca i64, align 8
  %a = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 11111 to ptr), i32 16)
       to label %na unwind label %u
na:
  %b = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 22222 to ptr), i32 16)
       to label %nb unwind label %u
nb:
  %ea = call hotspotcc i1 @jeandle.monitorenter_with_lightweight_lock(
                  ptr addrspace(1) %a, ptr %lock_a), !jeandle.lock_depth !{i32 0}
  %eb = call hotspotcc i1 @jeandle.monitorenter_with_lightweight_lock(
                  ptr addrspace(1) %b, ptr %lock_b), !jeandle.lock_depth !{i32 1}
  call void @sink(ptr addrspace(1) %b)
  %xb = call hotspotcc i1 @jeandle.monitorexit_with_lightweight_lock(
                  ptr addrspace(1) %b, ptr %lock_b)
  %xa = call hotspotcc i1 @jeandle.monitorexit_with_lightweight_lock(
                  ptr addrspace(1) %a, ptr %lock_a)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; Cascade fires: both A and B materialize.
; CHECK-LABEL: define void @test_lockdepth_cascade_metadata
; CHECK-DAG: %[[MATA:[A-Za-z0-9._]+]] = invoke hotspotcc{{.*}}ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 11111 to ptr), i32 16)
; CHECK-DAG: %[[MATB:[A-Za-z0-9._]+]] = invoke hotspotcc{{.*}}ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 22222 to ptr), i32 16)
; CHECK: call hotspotcc i1 @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1) %[[MATA]],
; CHECK: call hotspotcc i1 @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1) %[[MATB]],
; CHECK: call void @sink(ptr addrspace(1) %[[MATB]])
; CHECK: call hotspotcc i1 @jeandle.monitorexit_with_lightweight_lock(ptr addrspace(1) %[[MATB]],
; CHECK: call hotspotcc i1 @jeandle.monitorexit_with_lightweight_lock(ptr addrspace(1) %[[MATA]],

!java-method-compilation = !{}
