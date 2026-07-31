; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" -verify-each %s -o /dev/null
; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s
; RUN: opt -disable-output -jeandle-trace-pea \
; RUN:   -passes="require<partial-escape-analysis>,partial-escape-transform" %s 2>&1 \
; RUN:   | FileCheck %s --check-prefix=TRACE

; Guarded RAUW for RAUWOrigToPHI. When PEA inserts a per-pred merge PHI
; (Case-A) for an OrigAlloc that escapes via lock mismatch, a blanket
; replaceAllUsesWith would substitute the new PHI into every remaining use
; of OrigAlloc — including any pre-existing PHI in the same (or
; downstream) merge block that references OrigAlloc on its incoming edges.
; The PEA-inserted PHI is defined in MergeBB and does not dominate the
; predecessor edges; retargeting a pre-existing PHI's incoming value to it
; produces an SSA dominance violation that opt -verify-each rejects.
;
; Only non-PHI users (or PHI users in the PEA PHI's own block that ARE the
; PEA PHI itself) are rewritten; pre-existing PHIs naming OrigAlloc are
; left alone and become poison-incomings when EliminateAllocation RAUWs
; OrigAlloc to PoisonValue (which is safe because the pre-existing PHI is
; itself dead-coded by the trivial-dead sweep).
; An external padding monitor on the else arm keeps the scalar CFG depth
; balanced without changing the virtual object's per-pred lock mismatch.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc void @jeandle.monitorenter_with_thin_lock(ptr addrspace(1), ptr) nounwind
declare hotspotcc void @jeandle.monitorexit_with_thin_lock(ptr addrspace(1), ptr) nounwind
declare i32 @__gxx_personality_v0(...)

define void @rauwphi_safe(i1 %c, ptr addrspace(1) %pad) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %lock = alloca i64, align 8
  %pad.lock = alloca i64, align 8
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %branch unwind label %u
branch:
  br i1 %c, label %then, label %else
then:
  ; The source tail marker distinguishes this enter from a canonical replay.
  tail call hotspotcc void @jeandle.monitorenter_with_thin_lock(
              ptr addrspace(1) %o, ptr %lock)
  br label %merge
else:
  call hotspotcc void @jeandle.monitorenter_with_thin_lock(
              ptr addrspace(1) %pad, ptr %pad.lock)
  br label %merge
merge:
  ; Pre-existing PHI naming OrigAlloc on both incoming preds. With a
  ; blanket RAUW the PEA-inserted PHI would have replaced both incomings,
  ; defining itself in terms of a value (itself) that doesn't dominate the
  ; predecessor edges. With the guarded RAUW the pre-existing PHI is left
  ; alone (and dead-coded by the trivial-dead sweep after
  ; EliminateAllocation).
  %pre = phi ptr addrspace(1) [ %o, %then ], [ %o, %else ]
  %held = phi ptr addrspace(1) [ %o, %then ], [ %pad, %else ]
  %held.lock = phi ptr [ %lock, %then ], [ %pad.lock, %else ]
  call hotspotcc void @jeandle.monitorexit_with_thin_lock(
              ptr addrspace(1) %held, ptr %held.lock)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; Round-trips through opt -verify-each — if a malformed PHI sneaks past the
; guarded RAUW, the verifier would abort and FileCheck would never see the
; label. The transformation itself is verified by the first RUN line; this
; FileCheck is just a smoke test that the materialisation pipeline still
; produces a per-pred allocation invoke.
; CHECK-LABEL: define void @rauwphi_safe
; CHECK: invoke hotspotcc{{.*}}@jeandle.new_instance
; CHECK-NOT: tail call hotspotcc void @jeandle.monitorenter_with_thin_lock
; CHECK: call hotspotcc void @jeandle.monitorenter_with_thin_lock
; TRACE: PEA: LockReplay function=@rauwphi_safe

!java-method-compilation = !{}
