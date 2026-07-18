; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Lock leak via per-pred lock re-emit on a shared pred (review §3 #4).
; `lockpath` holds an unbalanced enter on %o and branches to `m1` (sink(o)
; escape) and `m2` (the matching monitorexit); `alt` (no lock) also reaches
; `m1`. Pre-fix, the per-pred materialize re-emitted the enter at
; lockpath's terminator (executing on the m2 path too) WITHOUT flipping the
; shared exit state, so m2 saw %o still virtual+locked and its monitorexit
; was folded away -> the m2 path acquired the lock and never released it.
; With the shared-flip (Case-A) for lock-carrying preds, m2 inherits the
; materialized view and the exit survives as a REAL exit, balancing the
; re-emitted enter.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1), ptr)
declare hotspotcc void @jeandle.monitorexit_with_lightweight_lock(ptr addrspace(1), ptr)
declare void @sink(ptr addrspace(1))
declare void @marker()
declare i32 @__gxx_personality_v0(...)

define void @per_pred_lock_leak(i1 %c0, i1 %c1) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %lk = alloca i64, align 8
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 11111 to ptr), i32 24)
       to label %n unwind label %u
n:
  br i1 %c0, label %lockpath, label %alt
lockpath:
  call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(
              ptr addrspace(1) %o, ptr %lk)
  call void @marker()
  br i1 %c1, label %m1, label %m2
alt:
  br label %m1
m1:
  call void @sink(ptr addrspace(1) %o)
  ret void
m2:
  call hotspotcc void @jeandle.monitorexit_with_lightweight_lock(
              ptr addrspace(1) %o, ptr %lk)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; The original enter is folded away; exactly ONE re-emitted enter (at
; lockpath's terminator, covering both m1 and m2 paths) and m2's exit
; SURVIVES as a real exit — every path is balanced.
; CHECK-LABEL: define void @per_pred_lock_leak(
; CHECK-NOT: pea.mat
; CHECK: call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1) %o, ptr %lk)
; CHECK-NOT: call hotspotcc void @jeandle.monitorenter_with_lightweight_lock
; CHECK: call hotspotcc void @jeandle.monitorexit_with_lightweight_lock(ptr addrspace(1) %o, ptr %lk)
; CHECK-NOT: poison

!java-method-compilation = !{}
