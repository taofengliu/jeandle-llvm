; RUN: opt -passes=safepoint-elimination -S < %s | FileCheck %s

; The two polls are separated by a load — a real (non-transparent) instruction.
; Both must stay; collapsing them would drop the deopt anchor covering the
; load's program point.

declare hotspotcc void @jeandle.safepoint_poll()

define void @not_adjacent(i64 %n, ptr %a) gc "safepoint-in-loop-example" {
entry:
  %cmp = icmp sgt i64 %n, 0
  br i1 %cmp, label %loop.header, label %exit

loop.header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop.latch ]
  %exit.cond = icmp slt i64 %iv, %n
  br i1 %exit.cond, label %loop.body, label %exit

loop.body:
  call hotspotcc void @jeandle.safepoint_poll()
  %v = load i32, ptr %a
  call hotspotcc void @jeandle.safepoint_poll()
  br label %loop.latch

loop.latch:
  %iv.next = add nsw i64 %iv, 1
  br label %loop.header

exit:
  ret void
}

!java-method-compilation = !{}

; CHECK-LABEL: @not_adjacent(
; CHECK:       loop.body:
; CHECK-NEXT:    call hotspotcc void @jeandle.safepoint_poll()
; CHECK-NEXT:    load i32, ptr %a
; CHECK-NEXT:    call hotspotcc void @jeandle.safepoint_poll()
; CHECK-NEXT:    br label %loop.latch
