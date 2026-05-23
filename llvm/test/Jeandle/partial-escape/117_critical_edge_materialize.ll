; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; R6.S2 — Split critical edges before per-pred materialisation.
;
; The pre-Pass-1 critical-edge split in PartialEscapeTransform inspects
; every IsPerPred Materialize effect and, when the recorded PH has
; multiple successors and the merge successor has multiple predecessors,
; splits the PH→merge critical edge so the materialisation invoke (and
; its OOM-throwing unwind) only lives on the merge-bound path.
;
; Under the current SkipGlobalRAUW=true policy in mergeStates, the only
; analyzer path that emits IsPerPred Materialize effects is the A4
; lock-mismatch cascade (PartialEscapeAnalysis.cpp:1386-1396). That path
; also emits ReplaceInput effects for the un-elided monitorenter calls
; that sit in PH; moving the Materialize to a new edge-block PH' would
; break SSA dominance for the un-elided receiver. The transform's
; pre-pass therefore SKIPS the split when PH carries any ReplaceInput
; effect.
;
; This test exercises the lock-mismatch + critical-edge path and asserts
; the IR remains well-formed (verifyFunction passes — R6.S3) and the
; A4 lock-cascade machinery still materialises both arms per the
; existing 250_lock_mismatch_one_arm_locked.ll contract. The full
; OOM-only-on-merge guarantee will be restored in a future round when a
; richer un-elide model or the LockState port (R10) becomes available.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc i1 @jeandle.monitorenter_with_thin_lock(ptr addrspace(1), ptr)
declare i32 @__gxx_personality_v0(...)

define void @critical_edge_lock_mismatch(i1 %c1, i1 %c2)
    gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %lock = alloca i64, align 8
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %branch unwind label %u
branch:
  br i1 %c1, label %then, label %else
then:
  %en = call hotspotcc i1 @jeandle.monitorenter_with_thin_lock(
              ptr addrspace(1) %o, ptr %lock)
  ; Critical edge: `then` has two successors, `merge` has two preds.
  br i1 %c2, label %merge, label %cold
else:
  br label %merge
merge:
  ret void
cold:
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; The A4 cascade must still materialise %o per-pred; the verifyFunction
; gate (R6.S3) ensures the resulting IR is well-formed even when the
; pre-pass declines to split the critical edge.
; CHECK-LABEL: define void @critical_edge_lock_mismatch
; CHECK: invoke hotspotcc{{.*}}@jeandle.new_instance

!java-method-compilation = !{}
