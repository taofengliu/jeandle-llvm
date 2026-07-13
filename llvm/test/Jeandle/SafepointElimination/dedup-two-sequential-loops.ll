; RUN: opt -passes=safepoint-elimination -S < %s | FileCheck %s

; Two independent loops in one function: each dedups within itself (header
; poll erased, latch poll kept and marked) with no cross-loop bleed.

declare hotspotcc void @jeandle.safepoint_poll()

define void @sequential(i64 %n, i64 %m) gc "safepoint-in-loop-example" {
entry:
  br label %first.header

first.header:
  %i = phi i64 [ 0, %entry ], [ %i.next, %first.latch ]
  call hotspotcc void @jeandle.safepoint_poll()
  %first.cond = icmp slt i64 %i, %n
  br i1 %first.cond, label %first.latch, label %between

first.latch:
  call hotspotcc void @jeandle.safepoint_poll()
  %i.next = add nsw i64 %i, 1
  br label %first.header

between:
  br label %second.header

second.header:
  %j = phi i64 [ 0, %between ], [ %j.next, %second.latch ]
  call hotspotcc void @jeandle.safepoint_poll()
  %second.cond = icmp slt i64 %j, %m
  br i1 %second.cond, label %second.latch, label %exit

second.latch:
  call hotspotcc void @jeandle.safepoint_poll()
  %j.next = add nsw i64 %j, 1
  br label %second.header

exit:
  ret void
}

!java-method-compilation = !{}

; CHECK-LABEL: @sequential(
; CHECK:       first.header:
; CHECK-NOT:     call hotspotcc void @jeandle.safepoint_poll
; CHECK:       first.latch:
; CHECK-NEXT:    call hotspotcc void @jeandle.safepoint_poll(){{.*}}!poll-coverage
; CHECK:       second.header:
; CHECK-NOT:     call hotspotcc void @jeandle.safepoint_poll
; CHECK:       second.latch:
; CHECK-NEXT:    call hotspotcc void @jeandle.safepoint_poll(){{.*}}!poll-coverage
