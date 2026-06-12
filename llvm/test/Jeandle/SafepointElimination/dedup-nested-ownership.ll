; RUN: opt -passes=safepoint-elimination -S < %s | FileCheck %s

; Nested loops: the outer loop has two own polls (header + latch) and dedups
; to the latch one; the inner loop's single poll belongs to the inner loop and
; must be neither counted as outer coverage nor deleted by the outer dedup.

declare hotspotcc void @jeandle.safepoint_poll()

define void @nested(i64 %n, i64 %m) gc "safepoint-in-loop-example" {
entry:
  br label %outer.header

outer.header:
  %i = phi i64 [ 0, %entry ], [ %i.next, %outer.latch ]
  call hotspotcc void @jeandle.safepoint_poll()
  br label %inner.header

inner.header:
  %j = phi i64 [ 0, %outer.header ], [ %j.next, %inner.header ]
  call hotspotcc void @jeandle.safepoint_poll()
  %j.next = add nsw i64 %j, 1
  %inner.cond = icmp slt i64 %j.next, %m
  br i1 %inner.cond, label %inner.header, label %outer.latch

outer.latch:
  call hotspotcc void @jeandle.safepoint_poll()
  %i.next = add nsw i64 %i, 1
  %outer.cond = icmp slt i64 %i.next, %n
  br i1 %outer.cond, label %outer.header, label %exit

exit:
  ret void
}

!java-method-compilation = !{}

; CHECK-LABEL: @nested(
; CHECK:       outer.header:
; CHECK-NOT:     call hotspotcc void @jeandle.safepoint_poll
; CHECK:       inner.header:
; CHECK:         call hotspotcc void @jeandle.safepoint_poll()
; CHECK-NOT:     !jeandle.poll_coverage
; CHECK:       outer.latch:
; CHECK-NEXT:    call hotspotcc void @jeandle.safepoint_poll(){{.*}}!jeandle.poll_coverage
