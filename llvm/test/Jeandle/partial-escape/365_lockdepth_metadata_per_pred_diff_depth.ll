; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Lock-stack identity mismatch at a diamond where each arm enters the SAME
; virtual %o via a DISTINCT call site, so the two arms carry DIFFERENT proxy
; depths (RPO visits %t before %e, so lock_t gets depth 0 and lock_e depth 1).
; The per-element compare
;   S[i].Call == RefStack[i].Call && S[i].BytecodeDepth == RefStack[i].BytecodeDepth
; sees a Call mismatch (lock_t vs lock_e) — and a BytecodeDepth mismatch
; (0 vs 1) — both of which would have routed to per-pred materialise in the
; old model.
;
; Under reuse-OrigAlloc the mismatch no longer drives per-pred materialization.
; The ORIGINAL allocation (OrigAlloc %o) is kept verbatim; each arm's surviving
; monitorenter stays in its original block with receiver OrigAlloc. No fresh
; materialization invoke is emitted, no PHI is built at the merge.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1), ptr)
declare hotspotcc void @jeandle.monitorexit_with_lightweight_lock(ptr addrspace(1), ptr)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_lockdepth_per_pred_diff_depth(i1 %cond) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %lock_t = alloca i64, align 8
  %lock_e = alloca i64, align 8
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 8765 to ptr), i32 16)
       to label %dispatch unwind label %u
dispatch:
  br i1 %cond, label %t, label %e
t:
  ; Then-arm: this enter is the only one on %o on this path. RPO visits %t
  ; first, so the proxy assigns it depth 0.
  call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(
                  ptr addrspace(1) %o, ptr %lock_t)
  br label %merge
e:
  ; Else-arm: a DISTINCT call site on %o. RPO visits %e after %t, so the
  ; proxy assigns it depth 1. Different call site, different depth.
  call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(
                  ptr addrspace(1) %o, ptr %lock_e)
  br label %merge
merge:
  call void @sink(ptr addrspace(1) %o)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @test_lockdepth_per_pred_diff_depth
; The original allocation invoke is RETAINED (no fresh materialization).
; CHECK: %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 8765 to ptr), i32 16)
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
