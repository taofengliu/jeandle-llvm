; RUN: opt -passes=safepoint-elimination -S < %s | FileCheck %s
; RUN: opt -passes=safepoint-elimination -jeandle-safepoint-chunk-iters=10000 -S < %s \
; RUN:   | FileCheck %s --check-prefix=BIGBUDGET

; Constant trip count 5000 exceeds the default budget (1000): the poll stays.
; Raising the budget over the trip count makes the same loop eligible — the
; bound is policy, not a hard-coded predicate.

declare hotspotcc void @jeandle.safepoint_poll()

define void @above_budget(ptr %a) gc "safepoint-in-loop-example" {
entry:
  br label %loop.header

loop.header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop.header ]
  call hotspotcc void @jeandle.safepoint_poll()
  %p = getelementptr inbounds i32, ptr %a, i64 %iv
  store i32 0, ptr %p
  %iv.next = add nuw nsw i64 %iv, 1
  %exit.cond = icmp slt i64 %iv.next, 5000
  br i1 %exit.cond, label %loop.header, label %exit

exit:
  ret void
}

!java-method-compilation = !{}

; CHECK-LABEL: @above_budget(
; CHECK: call hotspotcc void @jeandle.safepoint_poll()

; BIGBUDGET-LABEL: @above_budget(
; BIGBUDGET-NOT: call hotspotcc void @jeandle.safepoint_poll
