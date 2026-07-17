; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Lock-count mismatch with a post-merge escape, under the reuse-OrigAlloc model.
;
;   then: monitorenter(o) tracked virtually — LockCount[o]=1.
;   else: no lock — LockCount[o]=0.
;   merge: counts disagree; the object escapes via sink(o).
;
; Under reuse-OrigAlloc the lock-count mismatch no longer drives a per-pred
; materialization cascade. The ORIGINAL allocation (OrigAlloc %o) is kept
; alive (it dominates the escape point), the single surviving monitorenter
; stays in its original block with receiver OrigAlloc, and the post-merge sink
; receives OrigAlloc directly. No fresh materialization invoke is emitted, no
; PHI is built at the merge.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc void @jeandle.monitorenter_with_thin_lock(ptr addrspace(1), ptr)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_lock_mismatch_then_escape(i1 %c) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
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
  call void @sink(ptr addrspace(1) %o)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @test_lock_mismatch_then_escape
; The original allocation invoke is RETAINED (no fresh materialization).
; CHECK: %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 12345 to ptr), i32 16)
; No pea.mat materialization invoke is emitted.
; CHECK-NOT: pea.mat = invoke
; The single surviving monitorenter stays in its original block, receiver
; OrigAlloc %o.
; CHECK: call hotspotcc void @jeandle.monitorenter_with_thin_lock(ptr addrspace(1) %o, ptr %lock)
; No PHI is synthesized at the merge; sink receives OrigAlloc directly.
; CHECK-NOT: phi ptr addrspace(1)
; CHECK: call void @sink(ptr addrspace(1) %o)
; No second enter (else side had none).
; CHECK-NOT: call hotspotcc void @jeandle.monitorenter_with_thin_lock

!java-method-compilation = !{}
