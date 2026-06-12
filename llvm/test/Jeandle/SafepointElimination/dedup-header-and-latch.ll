; RUN: opt -passes=safepoint-elimination -S < %s | FileCheck %s

; Two polls both dominate the latch (header and latch). Keep-one keeps the
; latch-closest — the one in the latch itself — and erases the header's.

declare hotspotcc void @jeandle.safepoint_poll()

define void @header_and_latch(i64 %n) gc "safepoint-in-loop-example" {
entry:
  br label %loop.header

loop.header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop.latch ]
  call hotspotcc void @jeandle.safepoint_poll()
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

; CHECK-LABEL: @header_and_latch(
; CHECK:       loop.header:
; CHECK-NOT:     call hotspotcc void @jeandle.safepoint_poll
; CHECK:       loop.latch:
; CHECK-NEXT:    call hotspotcc void @jeandle.safepoint_poll(){{.*}}!jeandle.poll_coverage
