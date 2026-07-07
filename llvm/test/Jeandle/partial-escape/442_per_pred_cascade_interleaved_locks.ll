; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Per-pred cascade with RE-ENTRANT interleaved locks, exercising the §1.2 fix
; (per-pred lock re-emit global depth sort).
;
; A forward field chain a.f = b, b.g = c (so per-pred materializing `a` cascades
; prerequisites `b` then `c`). `a`, `b`, `c` are distinct new_instance klasses
; (11111/22222/33333). Branch left (no locks) / right with FOUR unbalanced
; jeandle.monitorenter_with_lightweight_lock calls. The depth comes from the
; analyzer's RPO-order proxy (no `!jeandle.lock_depth` metadata): the four
; right-block enters get proxy depths 0,1,2,3 — re-entrant on `a` (depths 0 and 2).
; NO monitorexits (unbalanced). The lock mismatch (right locks these objects,
; left does not) drives per-pred materialize of `a` (cascading `b`, `c`) on the
; RIGHT edge; all three per-pred Materialize effects share one escape point
; (the right pred's terminator), so they form one cascade group.
;
; Cascade SeqNo order (field-prereq recursion): c (deepest prereq, lowest SeqNo)
; -> b -> a (tail, highest SeqNo). Per-effect emit (the BUG) emits each effect's
; own Locks in SeqNo order: c's [c@3], b's [b@1], a's [a@0, a@2] => depths
; 3,1,0,2 — NOT strictly increasing, violating the lightweight-lock thread-stack
; contract. The fix merges the group's locks and globally depth-sorts them, then
; emits once from the tail (a): 0,1,2,3.
;
; Graal analog: one CommitAllocationNode per materialize point flattens every
; object's locks and lowers them globally depth-sorted
; (DefaultJavaLoweringProvider.java:1132,1149;
; PartialEscapeBlockState.java:267-269).

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1), ptr)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @per_pred_cascade_interleaved_locks(i1 %c)
    gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %la0 = alloca i64, align 8
  %lb1 = alloca i64, align 8
  %la2 = alloca i64, align 8
  %lc3 = alloca i64, align 8
  %a = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
              ptr inttoptr (i64 11111 to ptr), i32 32)
           to label %na unwind label %u
na:
  %b = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
              ptr inttoptr (i64 22222 to ptr), i32 32)
           to label %nb unwind label %u
nb:
  %c_obj = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
              ptr inttoptr (i64 33333 to ptr), i32 32)
           to label %fld unwind label %u
fld:
  %af = getelementptr inbounds i8, ptr addrspace(1) %a, i64 8
  store atomic ptr addrspace(1) %b, ptr addrspace(1) %af unordered, align 8
  %bg = getelementptr inbounds i8, ptr addrspace(1) %b, i64 16
  store atomic ptr addrspace(1) %c_obj, ptr addrspace(1) %bg unordered, align 8
  br label %branch
branch:
  br i1 %c, label %left, label %right
left:
  br label %merge
right:
  call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(
              ptr addrspace(1) %a, ptr %la0)
  call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(
              ptr addrspace(1) %b, ptr %lb1)
  call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(
              ptr addrspace(1) %a, ptr %la2)
  call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(
              ptr addrspace(1) %c_obj, ptr %lc3)
  br label %merge
merge:
  call void @sink(ptr addrspace(1) %a)
  call void @sink(ptr addrspace(1) %b)
  call void @sink(ptr addrspace(1) %c_obj)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

!java-method-compilation = !{}

; CHECK-LABEL: define void @per_pred_cascade_interleaved_locks
; The four re-emitted monitorenters must appear in strictly increasing lock
; depth on the right materialization path: a@0 (la0), b@1 (lb1), a@2 (la2,
; re-entrant on the SAME object as la0), c@3 (lc3). Before the fix each VO's
; locks were re-emitted together per-effect in SeqNo order (c@3, b@1, a@0,
; a@2 => depths 3,1,0,2), violating the lightweight-lock thread-stack contract.
; Sequential CHECK is order-sensitive — this IS the increasing-order assertion.
; CHECK: call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1) %{{[^,]+}}, ptr %la0)
; CHECK: call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1) %{{[^,]+}}, ptr %lb1)
; CHECK: call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1) %{{[^,]+}}, ptr %la2)
; CHECK: call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1) %{{[^,]+}}, ptr %lc3)
; CHECK-NOT: poison
