; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Case-A + locks + a folded JavaOp-invoke terminator, under the reuse-OrigAlloc
; model. The object %o has a tracked monitorenter in block `n` (before the
; branch); `else`'s terminator is a folded arraylength invoke on %o (Case-A
; materialize at the erased terminator). The eager-update hook
; (`relocateDependentMaterializes`) re-aims the Materialize's InsertBefore off
; the erased invoke onto the in-block successor (the `br`).
;
; Under reuse-OrigAlloc the original allocation invoke (OrigAlloc %o) is KEPT
; (no fresh pea.mat invoke). The folded arraylength invoke is erased. The
; tracked monitorenter — which was virtually elided from block `n` — is
; re-emitted ONCE at the escape point (in `else`, before the `br` to %merge),
; with receiver OrigAlloc %o. On the `then` path the object never escapes
; (the merge PHI picks `null`), so the lock is correctly NOT acquired there
; (partial-escape elides the lock on the no-escape path).

declare hotspotcc ptr addrspace(1) @jeandle.new_array(ptr, i32, i32, i32, i32)
declare hotspotcc i32 @jeandle.arraylength(ptr addrspace(1) readonly)
declare hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1), ptr)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @casea_folded_invoke_term_locks(i1 %c) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %lo = alloca i64, align 8
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_array(ptr inttoptr (i64 12345 to ptr), i32 7, i32 44, i32 16, i32 1048576)
         to label %n unwind label %u
n:
  call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1) %o, ptr %lo)
  br i1 %c, label %then, label %else
then:
  br label %merge
else:
  %len = invoke hotspotcc i32 @jeandle.arraylength(ptr addrspace(1) %o) to label %merge unwind label %handler
merge:
  %p = phi ptr addrspace(1) [ null, %then ], [ %o, %else ]
  call void @sink(ptr addrspace(1) %p)
  ret void
handler:
  %lp = landingpad i64 cleanup
  resume i64 %lp
u:
  %lpr = landingpad i64 cleanup
  resume i64 %lpr
}

!java-method-compilation = !{}

; CHECK-LABEL: define void @casea_folded_invoke_term_locks
; The folded arraylength invoke is erased.
; CHECK-NOT: @jeandle.arraylength
; The ORIGINAL allocation invoke (OrigAlloc %o) is RETAINED.
; CHECK: %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_array(ptr inttoptr (i64 12345 to ptr), i32 7, i32 44, i32 16, i32 1048576)
; No pea.mat materialization invoke is emitted.
; CHECK-NOT: pea.mat = invoke
; The tracked monitorenter is re-emitted exactly once at the escape point
; (in the else block, before the br to merge), receiver OrigAlloc %o.
; CHECK: else:
; CHECK-NOT: call hotspotcc void @jeandle.monitorenter_with_lightweight_lock
; CHECK: call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1) %o, ptr %lo)
; CHECK-NOT: call hotspotcc void @jeandle.monitorenter_with_lightweight_lock
; The merge PHI and sink are preserved (sink receives the PHI).
; CHECK: %p = phi ptr addrspace(1) [ null, %then ], [ %o, %else ]
; CHECK: call void @sink(ptr addrspace(1) %p)
