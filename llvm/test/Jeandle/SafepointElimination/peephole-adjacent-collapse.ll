; RUN: opt -passes=safepoint-elimination -S < %s | FileCheck %s

; Two `jeandle.safepoint_poll` calls sit back to back on the same straight-line
; block. They collapse to one, keeping the later.

declare hotspotcc void @jeandle.safepoint_poll()

define void @adjacent_polls(i64 %n) gc "safepoint-in-loop-example" {
entry:
  %cmp = icmp sgt i64 %n, 0
  br i1 %cmp, label %loop.header, label %exit

loop.header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop.latch ]
  %exit.cond = icmp slt i64 %iv, %n
  br i1 %exit.cond, label %loop.body, label %exit

loop.body:
  call hotspotcc void @jeandle.safepoint_poll()
  call hotspotcc void @jeandle.safepoint_poll()
  br label %loop.latch

loop.latch:
  %iv.next = add nsw i64 %iv, 1
  br label %loop.header

exit:
  ret void
}

!java-method-compilation = !{}

; CHECK-LABEL: @adjacent_polls(
; CHECK:       loop.body:
; CHECK-NEXT:    call hotspotcc void @jeandle.safepoint_poll()
; CHECK-NEXT:    br label %loop.latch
