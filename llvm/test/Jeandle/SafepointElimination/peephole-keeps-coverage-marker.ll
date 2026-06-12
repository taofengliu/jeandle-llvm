; RUN: opt -passes=safepoint-elimination -S < %s | FileCheck %s

; A poll tagged !jeandle.poll_coverage is its loop's designated coverage and
; must survive adjacent collapse in both orderings: marked-then-plain would by
; the positional rule keep the plain (later) one, so the marker must override.

declare hotspotcc void @jeandle.safepoint_poll()

define void @plain_then_marked(i64 %n) gc "safepoint-in-loop-example" {
entry:
  br label %loop.header

loop.header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop.header ]
  call hotspotcc void @jeandle.safepoint_poll()
  call hotspotcc void @jeandle.safepoint_poll(), !jeandle.poll_coverage !0
  %iv.next = add nsw i64 %iv, 1
  %exit.cond = icmp slt i64 %iv.next, %n
  br i1 %exit.cond, label %loop.header, label %exit

exit:
  ret void
}

define void @marked_then_plain(i64 %n) gc "safepoint-in-loop-example" {
entry:
  br label %loop.header

loop.header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop.header ]
  call hotspotcc void @jeandle.safepoint_poll(), !jeandle.poll_coverage !0
  call hotspotcc void @jeandle.safepoint_poll()
  %iv.next = add nsw i64 %iv, 1
  %exit.cond = icmp slt i64 %iv.next, %n
  br i1 %exit.cond, label %loop.header, label %exit

exit:
  ret void
}

!java-method-compilation = !{}
!0 = !{}

; CHECK-LABEL: @plain_then_marked(
; CHECK:       loop.header:
; CHECK:         call hotspotcc void @jeandle.safepoint_poll(){{.*}}!jeandle.poll_coverage
; CHECK-NOT:     call hotspotcc void @jeandle.safepoint_poll
; CHECK:         br i1

; CHECK-LABEL: @marked_then_plain(
; CHECK:       loop.header:
; CHECK:         call hotspotcc void @jeandle.safepoint_poll(){{.*}}!jeandle.poll_coverage
; CHECK-NOT:     call hotspotcc void @jeandle.safepoint_poll
; CHECK:         br i1
