; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Lock-stack identity mismatch at a diamond merge, under the reuse-OrigAlloc model.
;
; A single virtual %o is allocated before the diamond. Each arm enters its OWN
; monitorenter call site on %o (lock_t and lock_e are distinct alloca slots
; used for two distinct enter call instances). At the merge, both preds report
; LockCount==1 on %o but the per-pred live stacks differ
; (CallSite_then != CallSite_else).
;
; Under reuse-OrigAlloc the lock-stack identity mismatch no longer routes
; through per-pred materialization. The ORIGINAL allocation (OrigAlloc %o)
; dominates every use and is kept verbatim; each arm's surviving monitorenter
; stays in its original block with receiver OrigAlloc. The post-merge sink
; receives OrigAlloc directly. No fresh materialization invoke is emitted and
; no PHI is built at the merge.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1), ptr)
declare hotspotcc void @jeandle.monitorexit_with_lightweight_lock(ptr addrspace(1), ptr)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_lockdepth_stack_identity_per_pred(i1 %cond) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %lock_t = alloca i64, align 8
  %lock_e = alloca i64, align 8
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 9999 to ptr), i32 16)
       to label %dispatch unwind label %u
dispatch:
  br i1 %cond, label %t, label %e
t:
  ; Then-arm enter on its own call site (proxy depth 0 — RPO visits %t first).
  call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(
                  ptr addrspace(1) %o, ptr %lock_t)
  br label %merge
e:
  ; Else-arm enter on a DIFFERENT call site (proxy depth 1 — %e is visited
  ; after %t). Call identity differs from the then-arm's enter, so locksEqual
  ; = false at the merge (the depth difference is incidental — the EnterCall
  ; mismatch alone would have routed to per-pred materialise in the old model).
  call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(
                  ptr addrspace(1) %o, ptr %lock_e)
  br label %merge
merge:
  ; Both preds hold one lock on %o; downstream observes OrigAlloc directly.
  call void @sink(ptr addrspace(1) %o)
  ; Note: there is no balanced monitorexit on the merged path on purpose;
  ; we are only checking the merge-time mat behaviour, and the per-arm enters
  ; survive in IR with their first operand unchanged on OrigAlloc.
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @test_lockdepth_stack_identity_per_pred
; The original allocation invoke is RETAINED (no fresh materialization).
; CHECK: %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 9999 to ptr), i32 16)
; No pea.mat materialization invoke is emitted.
; CHECK-NOT: pea.mat = invoke
; Each arm's enter call stays in its original block, receiver OrigAlloc %o,
; each using its own lock alloca.
; CHECK: call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1) %o, ptr %lock_t)
; CHECK: call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1) %o, ptr %lock_e)
; No PHI at the merge; sink receives OrigAlloc directly.
; CHECK-NOT: phi ptr addrspace(1)
; CHECK: call void @sink(ptr addrspace(1) %o)

!java-method-compilation = !{}
