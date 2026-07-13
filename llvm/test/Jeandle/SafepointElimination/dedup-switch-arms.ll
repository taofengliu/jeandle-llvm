; RUN: opt -passes=safepoint-elimination -S < %s | FileCheck %s

; Polls in switch arms are redundant when a poll in the latch dominates every
; complete iteration: keep-one erases the arm polls and tags the latch poll as
; the loop's designated coverage.

declare hotspotcc void @jeandle.safepoint_poll()

define void @switch_arms(i64 %n) gc "safepoint-in-loop-example" {
entry:
  br label %loop.header

loop.header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop.latch ]
  %rem = urem i64 %iv, 3
  switch i64 %rem, label %arm.default [
    i64 0, label %arm.a
    i64 1, label %arm.b
  ]

arm.a:
  call hotspotcc void @jeandle.safepoint_poll()
  br label %loop.latch

arm.b:
  call hotspotcc void @jeandle.safepoint_poll()
  br label %loop.latch

arm.default:
  br label %loop.latch

loop.latch:
  call hotspotcc void @jeandle.safepoint_poll()
  %iv.next = add nsw i64 %iv, 1
  %exit.cond = icmp slt i64 %iv.next, %n
  br i1 %exit.cond, label %loop.header, label %exit

exit:
  ret void
}

!java-method-compilation = !{}

; CHECK-LABEL: @switch_arms(
; CHECK:       arm.a:
; CHECK-NEXT:    br label %loop.latch
; CHECK:       arm.b:
; CHECK-NEXT:    br label %loop.latch
; CHECK:       loop.latch:
; CHECK-NEXT:    call hotspotcc void @jeandle.safepoint_poll(){{.*}}!poll-coverage
