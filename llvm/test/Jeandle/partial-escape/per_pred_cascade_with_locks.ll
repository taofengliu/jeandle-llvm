; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Per-pred replay of a CYCLIC field graph, under the reuse-OrigAlloc model.
; `o` is virtual on both merge preds with lock counts 0 (left) / 1 (right) — a
; lock mismatch — and `o` and `p` form a cycle: o.f = p, p.g = o. The mismatch
; drives per-pred replay of the field stores at BOTH preds, but under
; reuse-OrigAlloc neither object is re-materialized: the ORIGINAL allocations
; (OrigAlloc %o and OrigAlloc %p) are both KEPT (no fresh pea.mat invokes) and
; each replayed field store uses OrigAlloc as its value. The back edge p.g = o
; resolves to OrigAlloc %o (no poison). The unbalanced monitorenter stays in
; its original block (`right`), receiver OrigAlloc %o — no per-pred
; materialization site exists to relocate it to.
; See PartialEscapeTransform.cpp applyMaterialize (cyclic-field-materialize).

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc void @jeandle.monitorenter_with_thin_lock(ptr addrspace(1), ptr)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @per_pred_cascade_with_locks(i1 %c)
    gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %lock = alloca i64, align 8
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 11111 to ptr), i32 16) to label %oi unwind label %u
oi:
  %p = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 22222 to ptr), i32 16) to label %fld unwind label %u
fld:
  %of = getelementptr inbounds i8, ptr addrspace(1) %o, i64 0
  store atomic ptr addrspace(1) %p, ptr addrspace(1) %of unordered, align 8
  %pg = getelementptr inbounds i8, ptr addrspace(1) %p, i64 8
  store atomic ptr addrspace(1) %o, ptr addrspace(1) %pg unordered, align 8
  br i1 %c, label %left, label %right
left:
  br label %merge
right:
  call hotspotcc void @jeandle.monitorenter_with_thin_lock(ptr addrspace(1) %o, ptr %lock)
  br label %merge
merge:
  call void @sink(ptr addrspace(1) %o)
  call void @sink(ptr addrspace(1) %p)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

!java-method-compilation = !{}

; CHECK-LABEL: define void @per_pred_cascade_with_locks
; Both ORIGINAL allocation invokes are RETAINED (no fresh materialization).
; CHECK: %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 11111 to ptr), i32 16)
; CHECK: %p = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 22222 to ptr), i32 16)
; No pea.mat materialization invoke is emitted.
; CHECK-NOT: pea.mat = invoke
; Per-pred replayed field stores use OrigAlloc as the stored value (the cycle
; back-edge p.g = o resolves to OrigAlloc %o, never poison). Two stores per
; pred: left emits both forward and back edges, right emits both too.
; CHECK: store atomic ptr addrspace(1) %o, ptr addrspace(1) %pea.matslot{{[0-9]*}} unordered, align 8
; CHECK: store atomic ptr addrspace(1) %p, ptr addrspace(1) %pea.matslot{{[0-9]*}} unordered, align 8
; CHECK: store atomic ptr addrspace(1) %o, ptr addrspace(1) %pea.matslot{{[0-9]*}} unordered, align 8
; CHECK: store atomic ptr addrspace(1) %p, ptr addrspace(1) %pea.matslot{{[0-9]*}} unordered, align 8
; CHECK-NOT: poison
; The single surviving monitorenter stays in its original block (`right`),
; receiver OrigAlloc %o (no re-emit at a per-pred materialization site).
; CHECK: call hotspotcc void @jeandle.monitorenter_with_thin_lock(ptr addrspace(1) %o, ptr %lock)
; Exactly one monitorenter (no duplication).
; CHECK-NOT: call hotspotcc void @jeandle.monitorenter_with_thin_lock
; Sinks at merge receive OrigAlloc directly (no PHI).
; CHECK-NOT: phi ptr addrspace(1)
; CHECK: call void @sink(ptr addrspace(1) %o)
; CHECK: call void @sink(ptr addrspace(1) %p)
; CHECK: ret void
