; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Lock-mismatch on A triggers the per-pred materialize;
; the then-pred's materialize for A then cascades through the narrow
; strict-lock-order rule onto B (B is locked at lower depth — depth 0 —
; while A is locked at higher depth — depth 1 — in the then path; the
; rule "other.minOrder < this.maxOrder" matches). B becomes Materialized
; at the then-pred via cascade. Meanwhile the else-pred has no locks on
; either A or B, so the materialize at else for A doesn't cascade B.
;
; Resulting state at the merge:
;   * A: materialized on both preds (ptr PHI built on next retry).
;   * B: materialized on then-pred (cascade), still virtual on else-pred.
;     The mixed-merge path picks this up on retry; B's then-pred enter is
;     un-elided in IR with its receiver retargeted to the then-pred's
;     NewInv for B (no SSA violation — MatPerBlock-based ReplaceInput).
;
; The else-pred B remains harmless: no live locks at else, no surviving
; enter to retarget, and the merge has no downstream use of B.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc void @jeandle.monitorenter_with_thin_lock(ptr addrspace(1), ptr)
declare i32 @__gxx_personality_v0(...)

define void @test_lock_mismatch_with_cascade(i1 %c) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %lock = alloca i64, align 8
  %oA = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
              ptr inttoptr (i64 11111 to ptr), i32 16)
       to label %allocB unwind label %u
allocB:
  %oB = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
              ptr inttoptr (i64 22222 to ptr), i32 16)
       to label %branch unwind label %u
branch:
  br i1 %c, label %then, label %else
then:
  call hotspotcc void @jeandle.monitorenter_with_thin_lock(
              ptr addrspace(1) %oB, ptr %lock)
  call hotspotcc void @jeandle.monitorenter_with_thin_lock(
              ptr addrspace(1) %oA, ptr %lock)
  br label %merge
else:
  br label %merge
merge:
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @test_lock_mismatch_with_cascade
; The then-pred materializes BOTH B (cascade) and A (direct), then
; MatCont chain replays both surviving enters with the right per-pred
; receivers. Klasses are 11111 for A and 22222 for B; the cascade order
; (B before A in the new IR layout because B's narrow-cascade rule fires
; first during A's materializeAtPredFromExitInfo call) puts klass 22222
; first.
; CHECK: invoke hotspotcc{{.*}}@jeandle.new_instance(ptr inttoptr (i64 22222 to ptr), i32 16)
; CHECK: invoke hotspotcc{{.*}}@jeandle.new_instance(ptr inttoptr (i64 11111 to ptr), i32 16)
; Surviving un-elided enters: one for B, one for A, both in the same
; per-pred MatCont chain.
; CHECK: call hotspotcc void @jeandle.monitorenter_with_thin_lock
; CHECK: call hotspotcc void @jeandle.monitorenter_with_thin_lock
; The else-pred materializes A only (no cascade for B because A has no
; live locks at else — the cascade rule needs HasLiveLocks). B at else stays
; virtual and is absorbed by the mixed-merge inherit-with-OrigAlloc path.
; CHECK: invoke hotspotcc{{.*}}@jeandle.new_instance(ptr inttoptr (i64 11111 to ptr), i32 16)
; No more enters past this point.
; CHECK-NOT: call hotspotcc void @jeandle.monitorenter_with_thin_lock

!java-method-compilation = !{}
