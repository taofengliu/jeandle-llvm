; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Multi-successor per-pred cascade with lock re-emit under the reuse-OrigAlloc
; model (review S1.2 F2 analog).
;
; `right` (locked) has TWO successor merges (merge1, merge2), both mixed
; (left arm virtual, right arm locked). The a.f=b field store is tracked, and
; materializing a per-pred cascades b (forward prerequisite). Each merge is an
; escape point for both a and b.
;
; Historically each (right, merge) critical edge was split into its own
; pea.crit.split, re-aiming the per-pred Materialize effects there, and the
; unified merged-emit fired once per split edge with locks depth-sorted
; (a@0, b@1) per split -- four enters in la,lb,la,lb order.
;
; Under reuse-OrigAlloc no edge is split: the original allocations %a, %b are
; retained, the a.f=b store is replayed onto %a (with %b) at each pred, and
; the right-arm locks are re-emitted onto %a/%b directly in `right` (two
; enters per merge: %a then %b, giving la,la,lb,lb). Both merges sink %a and
; %b directly. No %pea.mat, no pea.crit.split.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1), ptr)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_multi_succ_per_pred_cascade_locks(i1 %c, i1 %c2, i1 %c3)
    gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %la = alloca i64, align 8
  %lb = alloca i64, align 8
  %a = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
          ptr inttoptr (i64 11111 to ptr), i32 16)
       to label %na unwind label %u
na:
  %b = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
          ptr inttoptr (i64 22222 to ptr), i32 16)
       to label %fld unwind label %u
fld:
  ; a.f = b: materializing a per-pred cascades b (forward prereq).
  %af = getelementptr inbounds i8, ptr addrspace(1) %a, i64 0
  store atomic ptr addrspace(1) %b, ptr addrspace(1) %af unordered, align 8
  br i1 %c, label %left, label %right
left:
  ; No locks; branches to BOTH merges so each merge is mixed (left virtual,
  ; right locked).
  br i1 %c3, label %merge1, label %merge2
right:
  ; Unbalanced enters on a (depth 0) and b (depth 1); two successors.
  call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(
      ptr addrspace(1) %a, ptr %la)
  call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(
      ptr addrspace(1) %b, ptr %lb)
  br i1 %c2, label %merge1, label %merge2
merge1:
  call void @sink(ptr addrspace(1) %a)
  call void @sink(ptr addrspace(1) %b)
  ret void
merge2:
  call void @sink(ptr addrspace(1) %a)
  call void @sink(ptr addrspace(1) %b)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @test_multi_succ_per_pred_cascade_locks
; Both original allocation invokes are retained (a, then b).
; CHECK: invoke hotspotcc{{.*}}@jeandle.new_instance(ptr inttoptr (i64 11111 to ptr), i32 16)
; CHECK: invoke hotspotcc{{.*}}@jeandle.new_instance(ptr inttoptr (i64 22222 to ptr), i32 16)
; No fresh materialization invokes anywhere.
; CHECK-NOT: pea.mat = invoke
; The a.f=b field store is replayed onto OrigAlloc %a with OrigAlloc %b.
; CHECK: store atomic ptr addrspace(1) %b, ptr addrspace(1) %{{.*}} unordered, align 8
; The right arm re-emits its unbalanced enters onto OrigAlloc %a/%b directly
; (two enters per merge: %a with %la, then %b with %lb).
; CHECK: call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1) %a, ptr %la)
; CHECK: call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1) %a, ptr %la)
; CHECK: call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1) %b, ptr %lb)
; CHECK: call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1) %b, ptr %lb)
; CHECK-NOT: poison
; CHECK-NOT: pea.crit.split
; CHECK: ret void

!java-method-compilation = !{}
