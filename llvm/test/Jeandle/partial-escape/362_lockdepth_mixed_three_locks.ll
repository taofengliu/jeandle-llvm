; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Lit coverage for narrow cascade with explicit mixed depths.
; Three locks; B (the middle one) escapes. With metadata depths
;   A.depth = 0, B.depth = 1, C.depth = 2
; the narrow rule
;   other.front().BytecodeDepth < this.back().BytecodeDepth
; selects A (A.minDepth=0 < B.maxDepth=1) but NOT C (C.minDepth=2,
; NOT < 1). A and B materialise; C stays virtual. Mirrors test
; 192_narrow_lock_cascade_three_deep.ll but explicitly metadata-driven.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc i1 @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1), ptr)
declare hotspotcc i1 @jeandle.monitorexit_with_lightweight_lock(ptr addrspace(1), ptr)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_lockdepth_mixed_three_locks() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %lock_a = alloca i64, align 8
  %lock_b = alloca i64, align 8
  %lock_c = alloca i64, align 8
  %a = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 55555 to ptr), i32 16)
       to label %na unwind label %u
na:
  %b = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 66666 to ptr), i32 16)
       to label %nb unwind label %u
nb:
  %c = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 77777 to ptr), i32 16)
       to label %nc unwind label %u
nc:
  %ea = call hotspotcc i1 @jeandle.monitorenter_with_lightweight_lock(
                  ptr addrspace(1) %a, ptr %lock_a), !jeandle.lock_depth !{i32 0}
  %eb = call hotspotcc i1 @jeandle.monitorenter_with_lightweight_lock(
                  ptr addrspace(1) %b, ptr %lock_b), !jeandle.lock_depth !{i32 1}
  %ec = call hotspotcc i1 @jeandle.monitorenter_with_lightweight_lock(
                  ptr addrspace(1) %c, ptr %lock_c), !jeandle.lock_depth !{i32 2}
  ; Middle B escapes.
  call void @sink(ptr addrspace(1) %b)
  %xc = call hotspotcc i1 @jeandle.monitorexit_with_lightweight_lock(
                  ptr addrspace(1) %c, ptr %lock_c)
  %xb = call hotspotcc i1 @jeandle.monitorexit_with_lightweight_lock(
                  ptr addrspace(1) %b, ptr %lock_b)
  %xa = call hotspotcc i1 @jeandle.monitorexit_with_lightweight_lock(
                  ptr addrspace(1) %a, ptr %lock_a)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; A and B materialize; C must NOT.
; CHECK-LABEL: define void @test_lockdepth_mixed_three_locks
; CHECK-DAG: %[[MATA:[A-Za-z0-9._]+]] = invoke hotspotcc{{.*}}ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 55555 to ptr), i32 16)
; CHECK-DAG: %[[MATB:[A-Za-z0-9._]+]] = invoke hotspotcc{{.*}}ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 66666 to ptr), i32 16)
; CHECK: call hotspotcc i1 @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1) %[[MATA]],
; CHECK: call hotspotcc i1 @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1) %[[MATB]],
; CHECK: call void @sink(ptr addrspace(1) %[[MATB]])
; CHECK: call hotspotcc i1 @jeandle.monitorexit_with_lightweight_lock(ptr addrspace(1) %[[MATB]],
; CHECK: call hotspotcc i1 @jeandle.monitorexit_with_lightweight_lock(ptr addrspace(1) %[[MATA]],
; C must not be materialized.
; CHECK-NOT: jeandle.new_instance(ptr inttoptr (i64 77777 to ptr)
; CHECK-NOT: jeandle.monitorenter_with_lightweight_lock{{.*}}%lock_c
; CHECK-NOT: jeandle.monitorexit_with_lightweight_lock{{.*}}%lock_c

!java-method-compilation = !{}
