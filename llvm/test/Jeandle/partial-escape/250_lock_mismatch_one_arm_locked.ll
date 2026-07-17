; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Lock-count mismatch at a merge, under the reuse-OrigAlloc model.
;
; entry: alloc o (virtual). branch on %c.
; then: monitorenter(o) — tracked virtually, LockCount[o]=1 at then exit.
; else: nothing — LockCount[o]=0 at else exit.
; merge: counts disagree.
;
; Under reuse-OrigAlloc the lock-count mismatch no longer drives a per-pred
; materialization cascade. The ORIGINAL allocation (OrigAlloc %o) dominates
; every escape point and every use, so it is kept verbatim and the surviving
; monitorenter stays in its original position with its receiver pointing at
; OrigAlloc. No fresh materialization invoke (pea.mat) is emitted, no PHI is
; built at the merge, and the entry allocation is not eliminated.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc void @jeandle.monitorenter_with_thin_lock(ptr addrspace(1), ptr)
declare i32 @__gxx_personality_v0(...)

define void @test_lock_mismatch_one_arm_locked(i1 %c) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
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
  br label %merge
merge:
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @test_lock_mismatch_one_arm_locked
; The original allocation invoke is RETAINED (no fresh materialization).
; CHECK: %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 12345 to ptr), i32 16)
; No pea.mat materialization invoke is emitted.
; CHECK-NOT: pea.mat = invoke
; The single surviving monitorenter stays in its original block, receiver
; OrigAlloc %o.
; CHECK: call hotspotcc void @jeandle.monitorenter_with_thin_lock(ptr addrspace(1) %o, ptr %lock)
; Exactly one monitorenter (no per-pred duplication, no synthesized enters).
; CHECK-NOT: call hotspotcc void @jeandle.monitorenter_with_thin_lock

!java-method-compilation = !{}
