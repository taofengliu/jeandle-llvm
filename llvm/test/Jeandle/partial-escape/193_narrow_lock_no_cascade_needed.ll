; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; PEA / C8: outer-lock holder escapes while inner-lock holder is still
; virtual. Under Graal's narrow cascade rule
;   other.minLockDepth < this.maxLockDepth
; we get B.min (1) NOT < A.max (0), so the inner virtual B is NOT
; cascaded — it stays virtual and folds away cleanly. The runtime
; LM_LIGHTWEIGHT lock stack after A materializes is just [A]; B's
; (still-virtual) monitorenter/exit pair never touches the real lock
; stack, so no disorder can occur.
;
; This is the case where C8 buys an actual win over the prior
; broad-cascade implementation (which would have over-materialized B).

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc i1 @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1), ptr)
declare hotspotcc i1 @jeandle.monitorexit_with_lightweight_lock(ptr addrspace(1), ptr)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_no_cascade_outer_escapes() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %lock_a = alloca i64, align 8
  %lock_b = alloca i64, align 8
  %a = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 88888 to ptr), i32 16)
       to label %na unwind label %u
na:
  %b = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 99999 to ptr), i32 16)
       to label %nb unwind label %u
nb:
  ; Acquire outer A.
  %ea = call hotspotcc i1 @jeandle.monitorenter_with_lightweight_lock(
                  ptr addrspace(1) %a, ptr %lock_a)
  ; Acquire inner B.
  %eb = call hotspotcc i1 @jeandle.monitorenter_with_lightweight_lock(
                  ptr addrspace(1) %b, ptr %lock_b)
  ; Outer A escapes.
  call void @sink(ptr addrspace(1) %a)
  %xb = call hotspotcc i1 @jeandle.monitorexit_with_lightweight_lock(
                  ptr addrspace(1) %b, ptr %lock_b)
  %xa = call hotspotcc i1 @jeandle.monitorexit_with_lightweight_lock(
                  ptr addrspace(1) %a, ptr %lock_a)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; A materializes; B must NOT.
; CHECK-LABEL: define void @test_no_cascade_outer_escapes
; CHECK: %[[MATA:[A-Za-z0-9._]+]] = invoke hotspotcc{{.*}}ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 88888 to ptr), i32 16)
; CHECK: call hotspotcc i1 @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1) %[[MATA]],
; CHECK: call void @sink(ptr addrspace(1) %[[MATA]])
; CHECK: call hotspotcc i1 @jeandle.monitorexit_with_lightweight_lock(ptr addrspace(1) %[[MATA]],
; B and its monitor calls must NOT survive.
; CHECK-NOT: jeandle.new_instance(ptr inttoptr (i64 99999 to ptr)
; CHECK-NOT: jeandle.monitorenter_with_lightweight_lock{{.*}}%lock_b
; CHECK-NOT: jeandle.monitorexit_with_lightweight_lock{{.*}}%lock_b

!java-method-compilation = !{}
