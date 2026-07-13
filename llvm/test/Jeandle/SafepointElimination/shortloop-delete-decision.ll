; RUN: opt -passes=safepoint-elimination -S < %s | FileCheck %s

; The deleteShortLoopPolls decision in one place: an innermost loop's back-edge
; poll is deleted only when SCEV proves a constant max backedge-taken count
; within the budget (default 1000), compared inclusively. Three outcomes:
;   - const bound on the boundary (BTC == 1000) -> deleted
;   - const bound one past it      (BTC == 1001) -> kept
;   - non-constant (parameter) bound             -> kept
; The budget is policy, not a hard-coded predicate; the knob flip lives in
; shortloop-above-budget-kept.ll, the non-innermost case in
; shortloop-nested-outer-kept.ll.

declare hotspotcc void @jeandle.safepoint_poll()

; Trip count 1001 (BTC exactly 1000) sits on the inclusive boundary: deletable.
define void @const_at_budget_deleted(ptr %a) gc "safepoint-in-loop-example" {
entry:
  br label %loop.header

loop.header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop.header ]
  call hotspotcc void @jeandle.safepoint_poll()
  %p = getelementptr inbounds i32, ptr %a, i64 %iv
  store i32 0, ptr %p
  %iv.next = add nuw nsw i64 %iv, 1
  %exit.cond = icmp slt i64 %iv.next, 1001
  br i1 %exit.cond, label %loop.header, label %exit

exit:
  ret void
}

; Trip count 1002 (BTC 1001): one past the boundary, poll kept.
define void @const_above_budget_kept(ptr %a) gc "safepoint-in-loop-example" {
entry:
  br label %loop.header

loop.header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop.header ]
  call hotspotcc void @jeandle.safepoint_poll()
  %p = getelementptr inbounds i32, ptr %a, i64 %iv
  store i32 0, ptr %p
  %iv.next = add nuw nsw i64 %iv, 1
  %exit.cond = icmp slt i64 %iv.next, 1002
  br i1 %exit.cond, label %loop.header, label %exit

exit:
  ret void
}

; Parameter bound: SCEV can't prove any constant trip count, so the poll stays
; (an unknown-bound loop without coverage can stall a safepoint for seconds).
define void @param_bound_kept(i64 %n) gc "safepoint-in-loop-example" {
entry:
  %cmp = icmp sgt i64 %n, 0
  br i1 %cmp, label %loop.header, label %exit

loop.header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop.header ]
  call hotspotcc void @jeandle.safepoint_poll()
  %iv.next = add nsw i64 %iv, 1
  %exit.cond = icmp slt i64 %iv.next, %n
  br i1 %exit.cond, label %loop.header, label %exit

exit:
  ret void
}

!java-method-compilation = !{}

; CHECK-LABEL: @const_at_budget_deleted(
; CHECK-NOT:   call hotspotcc void @jeandle.safepoint_poll

; CHECK-LABEL: @const_above_budget_kept(
; CHECK:       call hotspotcc void @jeandle.safepoint_poll()

; CHECK-LABEL: @param_bound_kept(
; CHECK:       call hotspotcc void @jeandle.safepoint_poll()
