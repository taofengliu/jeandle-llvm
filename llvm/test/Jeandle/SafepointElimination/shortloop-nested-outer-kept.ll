; RUN: opt -passes=safepoint-elimination -S < %s | FileCheck %s

; The outer loop has a provable trip count of 10 but contains another loop, so
; it is not innermost and keeps its poll: with the (unbounded) inner loop in
; the body, "10 iterations" says nothing about time between polls, and a
; poll-free short inner loop inside a poll-free short outer loop would
; compound to budget^2 iterations.

declare hotspotcc void @jeandle.safepoint_poll()

define void @nested_outer(i64 %m) gc "safepoint-in-loop-example" {
entry:
  br label %outer.header

outer.header:
  %i = phi i64 [ 0, %entry ], [ %i.next, %outer.latch ]
  br label %inner.header

inner.header:
  %j = phi i64 [ 0, %outer.header ], [ %j.next, %inner.header ]
  call hotspotcc void @jeandle.safepoint_poll()
  %j.next = add nsw i64 %j, 1
  %inner.cond = icmp slt i64 %j.next, %m
  br i1 %inner.cond, label %inner.header, label %outer.latch

outer.latch:
  call hotspotcc void @jeandle.safepoint_poll()
  %i.next = add nuw nsw i64 %i, 1
  %outer.cond = icmp slt i64 %i.next, 10
  br i1 %outer.cond, label %outer.header, label %exit

exit:
  ret void
}

!java-method-compilation = !{}

; CHECK-LABEL: @nested_outer(
; CHECK:       inner.header:
; CHECK:         call hotspotcc void @jeandle.safepoint_poll()
; CHECK:       outer.latch:
; CHECK-NEXT:    call hotspotcc void @jeandle.safepoint_poll()
