; RUN: opt -passes=safepoint-elimination -S < %s | FileCheck %s

; Polls live only in the two branch arms; neither dominates the latch.
; Deleting either could leave the other path's iterations uncovered, so
; keep-one must delete nothing (C2: no dominating safepoint found -> keep all).

declare hotspotcc void @jeandle.safepoint_poll()

define void @no_dominating(i64 %n, i1 %c) gc "safepoint-in-loop-example" {
entry:
  br label %loop.header

loop.header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop.latch ]
  br i1 %c, label %then, label %else

then:
  call hotspotcc void @jeandle.safepoint_poll()
  br label %loop.latch

else:
  call hotspotcc void @jeandle.safepoint_poll()
  br label %loop.latch

loop.latch:
  %iv.next = add nsw i64 %iv, 1
  %exit.cond = icmp slt i64 %iv.next, %n
  br i1 %exit.cond, label %loop.header, label %exit

exit:
  ret void
}

!java-method-compilation = !{}

; CHECK-LABEL: @no_dominating(
; CHECK:       then:
; CHECK-NEXT:    call hotspotcc void @jeandle.safepoint_poll()
; CHECK:       else:
; CHECK-NEXT:    call hotspotcc void @jeandle.safepoint_poll()
