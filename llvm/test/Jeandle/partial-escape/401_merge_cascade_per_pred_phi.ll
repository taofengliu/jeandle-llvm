; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Lock-cascade merge under the reuse-OrigAlloc model. Two preds join at
; %merge with lock counts 0 (left) and 1 (right) on the same virtual object
; -- a lock disagreement that historically fired per-pred materialization at
; every pred (each emitting its own fresh alloc invoke and selecting between
; them with a materializedValuePhi).
;
; Under reuse-OrigAlloc the original allocation invoke (%o) is the SINGLE
; retained value: it dominates both preds and the merge. The right-arm lock
; (LC=1) is already on %o in the IR, so no per-pred materialize fires, no
; fresh alloc invoke is emitted, and no materialized-object PHI is needed.
; The return consumes OrigAlloc directly. (Field-value PHIs would still be
; built here for genuine per-offset field disagreements, but this scenario
; has none.)

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc void @jeandle.monitorenter_with_thin_lock(ptr addrspace(1), ptr)
declare i32 @__gxx_personality_v0(...)

define ptr addrspace(1) @test_merge_cascade_per_pred_phi(i1 %c)
    gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %lock = alloca i64, align 8
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %branch unwind label %u
branch:
  br i1 %c, label %left, label %right
left:
  br label %merge
right:
  call hotspotcc void @jeandle.monitorenter_with_thin_lock(
              ptr addrspace(1) %o, ptr %lock)
  br label %merge
merge:
  ret ptr addrspace(1) %o
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define ptr addrspace(1) @test_merge_cascade_per_pred_phi
; Exactly one allocation invoke (the original, retained) for the whole fn.
; CHECK: invoke hotspotcc{{.*}}@jeandle.new_instance(ptr inttoptr (i64 12345 to ptr), i32 16)
; CHECK-NOT: invoke hotspotcc{{.*}}@jeandle.new_instance
; The single right-arm lock is already on OrigAlloc %o -- no per-pred mat,
; no fresh invoke, no un-elided enter snap to a NewInv.
; CHECK: call hotspotcc void @jeandle.monitorenter_with_thin_lock(ptr addrspace(1) %o, ptr %lock)
; No materialized-object PHI and no critical-edge split: the return consumes
; OrigAlloc directly. (A bare CHECK-NOT: %pea.mat would false-match the
; replay slot %pea.matslot, so assert on the invoke form and on crit.split.)
; CHECK-NOT: pea.mat = invoke
; CHECK-NOT: pea.crit.split
; CHECK: ret ptr addrspace(1) %o

!java-method-compilation = !{}
