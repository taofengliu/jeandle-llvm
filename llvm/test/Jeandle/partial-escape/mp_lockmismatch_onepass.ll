; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; MergeProcessor / lock-count mismatch (1 vs 2), under the reuse-OrigAlloc model.
;
; Both arms lock the same virtual object but with different counts
; (then=1 enter, else=2 enters). The counts disagree at the merge, but under
; reuse-OrigAlloc the mismatch no longer drives a per-pred materialization +
; materializedValuePhi cascade. The ORIGINAL allocation (OrigAlloc %o) is kept
; verbatim, each arm's surviving monitorenters stay in their original blocks
; with receivers pointing at OrigAlloc, and the post-merge sink receives
; OrigAlloc directly. No fresh materialization invoke is emitted and no PHI is
; built at the merge.
;
; This is a regression anchor for the collapsed single-pass
; materializeAndBuildPhi (the pre-refactor code handled lock mismatch as a
; two-pass materialize-then-PHI across fixpoint iterations). 250 (0 vs 1)
; and 254 (0/1/2 over three preds) cover adjacent counts; this covers the
; 1-vs-2 two-pred shape.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc void @jeandle.monitorenter_with_thin_lock(ptr addrspace(1), ptr)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_lockmismatch_1v2(i1 %c) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %lock = alloca i64, align 8
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %branch unwind label %u
branch:
  br i1 %c, label %then, label %else
then:
  call hotspotcc void @jeandle.monitorenter_with_thin_lock(
              ptr addrspace(1) %o, ptr %lock)
  br label %merge
else:
  call hotspotcc void @jeandle.monitorenter_with_thin_lock(
              ptr addrspace(1) %o, ptr %lock)
  call hotspotcc void @jeandle.monitorenter_with_thin_lock(
              ptr addrspace(1) %o, ptr %lock)
  br label %merge
merge:
  call void @sink(ptr addrspace(1) %o)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @test_lockmismatch_1v2
; The original allocation invoke is RETAINED (no fresh materialization).
; CHECK: %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 12345 to ptr), i32 16)
; No pea.mat materialization invoke is emitted.
; CHECK-NOT: pea.mat = invoke
; then-arm's single enter stays in its original block, receiver OrigAlloc %o.
; CHECK: call hotspotcc void @jeandle.monitorenter_with_thin_lock(ptr addrspace(1) %o, ptr %lock)
; else-arm's two enters stay in their original block, receiver OrigAlloc %o.
; CHECK: call hotspotcc void @jeandle.monitorenter_with_thin_lock(ptr addrspace(1) %o, ptr %lock)
; CHECK: call hotspotcc void @jeandle.monitorenter_with_thin_lock(ptr addrspace(1) %o, ptr %lock)
; Exactly three monitorenters total — no synthesized enters anywhere.
; CHECK-NOT: call hotspotcc void @jeandle.monitorenter_with_thin_lock
; No PHI at the merge; sink receives OrigAlloc directly.
; CHECK-NOT: phi ptr addrspace(1)
; CHECK: call void @sink(ptr addrspace(1) %o)

!java-method-compilation = !{}
