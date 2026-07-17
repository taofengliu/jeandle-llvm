; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Three preds joining at the same merge with lock counts 0 / 1 / 2 on the
; same virtual object, under the reuse-OrigAlloc model.
;
;   c0: no enter         — LockCount=0.
;   c1: 1 enter          — LockCount=1.
;   c2: 2 enters         — LockCount=2.
;
; Under reuse-OrigAlloc the mismatch no longer drives a per-pred
; materialization cascade. The ORIGINAL allocation (OrigAlloc %o) dominates
; every use and is kept verbatim; each pred's surviving monitorenters stay in
; their original blocks with receivers pointing at OrigAlloc. No fresh
; materialization invoke is emitted, no enters are synthesized, and no PHI is
; built at the merge — the post-merge sink receives OrigAlloc directly.
; User semantics preserved exactly: 0 enters on c0, 1 enter on c1, 2 enters
; on c2.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc void @jeandle.monitorenter_with_thin_lock(ptr addrspace(1), ptr)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_lock_mismatch_three_preds(i32 %sel) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %lock = alloca i64, align 8
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %switchblk unwind label %u
switchblk:
  switch i32 %sel, label %c0 [ i32 1, label %c1
                               i32 2, label %c2 ]
c0:
  br label %merge
c1:
  call hotspotcc void @jeandle.monitorenter_with_thin_lock(
              ptr addrspace(1) %o, ptr %lock)
  br label %merge
c2:
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

; CHECK-LABEL: define void @test_lock_mismatch_three_preds
; The original allocation invoke is RETAINED (no fresh materialization).
; CHECK: %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 12345 to ptr), i32 16)
; No pea.mat materialization invoke is emitted.
; CHECK-NOT: pea.mat = invoke
; c1's single enter stays in its original block, receiver OrigAlloc %o.
; CHECK: call hotspotcc void @jeandle.monitorenter_with_thin_lock(ptr addrspace(1) %o, ptr %lock)
; c2's two enters stay in their original block, receiver OrigAlloc %o.
; CHECK: call hotspotcc void @jeandle.monitorenter_with_thin_lock(ptr addrspace(1) %o, ptr %lock)
; CHECK: call hotspotcc void @jeandle.monitorenter_with_thin_lock(ptr addrspace(1) %o, ptr %lock)
; Exactly three monitorenters total — no synthesized enters anywhere.
; CHECK-NOT: call hotspotcc void @jeandle.monitorenter_with_thin_lock
; No PHI is synthesized at the merge; sink receives OrigAlloc directly.
; CHECK-NOT: phi ptr addrspace(1)
; CHECK: call void @sink(ptr addrspace(1) %o)

!java-method-compilation = !{}
