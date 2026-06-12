; RUN: opt -passes=safepoint-elimination -S < %s | FileCheck %s

; Pins the budget boundary semantics: the bound compares the max
; BACKEDGE-taken count against N (default 1000), inclusively. Trip count 1001
; (BTC exactly 1000) is deletable; trip count 1002 (BTC 1001) is not.

declare hotspotcc void @jeandle.safepoint_poll()

define void @btc_equals_budget(ptr %a) gc "safepoint-in-loop-example" {
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

define void @btc_above_budget(ptr %a) gc "safepoint-in-loop-example" {
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

!java-method-compilation = !{}

; CHECK-LABEL: @btc_equals_budget(
; CHECK-NOT: call hotspotcc void @jeandle.safepoint_poll

; CHECK-LABEL: @btc_above_budget(
; CHECK: call hotspotcc void @jeandle.safepoint_poll()
