; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; R6.S9 + R10.X1c: materializeVirtualLocksBefore pre-cascade keyed on
; bytecode depth metadata. We have two virtuals %a and %b. Enter A at
; depth=0 (outer). Then enter B at depth=1 (inner). The pre-cascade fires
; when we are ABOUT to push B's enter: A's front depth (0) < new depth
; (1), so A must be materialised at B's enter site BEFORE the lock
; counter for B is bumped. Without R6.S9, an escape of A downstream would
; materialise A alone — leaving B's enter on the virtual lock stack
; corrupted because A's real monitorenter wouldn't be present in IR.
;
; After R6.S9+R10.X1c, A materialises at B's enter; both A's enter and
; the un-elide ReplaceInput emit a real call site for A. B stays virtual
; until the actual sink (which only references A in this test); B has
; nothing observable to materialise for, and its enter folds away.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc i1 @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1), ptr)
declare hotspotcc i1 @jeandle.monitorexit_with_lightweight_lock(ptr addrspace(1), ptr)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_lockdepth_pre_cascade_metadata() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %lock_a = alloca i64, align 8
  %lock_b = alloca i64, align 8
  %a = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 31415 to ptr), i32 16)
       to label %na unwind label %u
na:
  %b = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 27182 to ptr), i32 16)
       to label %nb unwind label %u
nb:
  %ea = call hotspotcc i1 @jeandle.monitorenter_with_lightweight_lock(
                  ptr addrspace(1) %a, ptr %lock_a), !jeandle.lock_depth !{i32 0}
  ; The next enter is at depth=1; pre-cascade selects A (A.front=0 < 1).
  %eb = call hotspotcc i1 @jeandle.monitorenter_with_lightweight_lock(
                  ptr addrspace(1) %b, ptr %lock_b), !jeandle.lock_depth !{i32 1}
  ; A leaks; B is unused downstream so B has no sink, but its narrow
  ; cascade would also fire here if anything escapes B.
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

; A is materialised (either at pre-cascade time or at the sink); A's enter
; survives on the materialised pointer; A's exit also survives. We do not
; over-constrain B's status — the conservative behaviour is for B to also
; materialise via the cascade-on-A path, but the post-conditions we PIN
; are A-only.
; CHECK-LABEL: define void @test_lockdepth_pre_cascade_metadata
; CHECK: %[[MATA:[A-Za-z0-9._]+]] = invoke hotspotcc{{.*}}ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 31415 to ptr), i32 16)
; CHECK: call hotspotcc i1 @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1) %[[MATA]],
; CHECK: call void @sink(ptr addrspace(1) %[[MATA]])
; CHECK: call hotspotcc i1 @jeandle.monitorexit_with_lightweight_lock(ptr addrspace(1) %[[MATA]],

!java-method-compilation = !{}
