; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; R6.S7 — Order CreatePHI vs same-block per-pred Materialize.
;
; Self-loop where the loop header IS its own back-edge predecessor. Without
; R6.S7, both the merge-block CreatePHI (lower SeqNo, emitted by
; mergeStates) and the per-pred Materialize for the back-edge (higher
; SeqNo, emitted by processBlockPhis) co-reside in BlockEffects[header].
; CreatePHI applied first → MatPerBlock empty → back-edge incoming wired
; to the OrigAlloc placeholder; Materialize applied second with
; IsPerPred=true → skipped global RAUW; Pass 2 EliminateAllocation RAUW'd
; OrigAlloc to PoisonValue → back-edge PHI incoming became `poison`.
;
; Post-fix: mergeStates DEFERS CreatePHI emission via PendingMergePhis;
; processBlock drains the pending list AFTER the instruction walk, so the
; CreatePHI is assigned a SeqNo strictly greater than the per-pred
; Materialize. The transform now applies Materialize first (populating
; MatPerBlock) and CreatePHI second (resolving its back-edge incoming
; through MatPerBlock to the per-pred NewInv).
;
; Test shape: alloc in entry, single-block loop body that stores to a
; field, escapes the receiver via @sink on the back-edge path, and exits
; otherwise.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @selfloop_perpred(i32 %n) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  br label %header
header:
  %i = phi i32 [ 0, %entry ], [ %i.next, %body ]
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %body unwind label %u
body:
  call void @sink(ptr addrspace(1) %o)
  %i.next = add i32 %i, 1
  %c = icmp slt i32 %i.next, %n
  br i1 %c, label %header, label %exit
exit:
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; The materialisation must produce a real new_instance invoke. The
; previously-poisoned phi incoming must not appear; if the analyzer were
; still wired through the placeholder path, FileCheck would see `poison`
; somewhere in the IR.
; CHECK-LABEL: define void @selfloop_perpred
; CHECK: invoke {{.*}}@jeandle.new_instance
; CHECK-NOT: phi {{.*}}poison
; CHECK-NOT: store{{.*}}poison

!java-method-compilation = !{}
