; RUN: opt -passes=safepoint-elimination -S < %s | FileCheck %s

; A coverage-marked poll in the header wins over a latch-closer plain poll:
; keep-one keeps the marked one and erases the plain. (The marker is
; pass-internal — set by a prior keep-one run or a future strip-mine anchor,
; not emitted by the frontend; pre-set here only to exercise the priority.)

declare hotspotcc void @jeandle.safepoint_poll()

define void @marked_priority(i64 %n) gc "safepoint-in-loop-example" {
entry:
  br label %loop.header

loop.header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop.latch ]
  call hotspotcc void @jeandle.safepoint_poll(), !jeandle.poll_coverage !0
  %exit.cond = icmp slt i64 %iv, %n
  br i1 %exit.cond, label %loop.latch, label %exit

loop.latch:
  call hotspotcc void @jeandle.safepoint_poll()
  %iv.next = add nsw i64 %iv, 1
  br label %loop.header

exit:
  ret void
}

!java-method-compilation = !{}
!0 = !{}

; CHECK-LABEL: @marked_priority(
; CHECK:       loop.header:
; CHECK:         call hotspotcc void @jeandle.safepoint_poll(){{.*}}!jeandle.poll_coverage
; CHECK:       loop.latch:
; CHECK-NOT:     call hotspotcc void @jeandle.safepoint_poll
; CHECK:         br label %loop.header
