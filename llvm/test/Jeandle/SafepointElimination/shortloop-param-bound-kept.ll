; RUN: opt -passes=safepoint-elimination -S < %s | FileCheck %s

; The bound is a parameter, so SCEV cannot prove a small trip count: the poll
; must stay (an unknown-bound loop without coverage can stall a safepoint for
; seconds).

declare hotspotcc void @jeandle.safepoint_poll()

define void @param_bound(i64 %n) gc "safepoint-in-loop-example" {
entry:
  %cmp = icmp sgt i64 %n, 0
  br i1 %cmp, label %loop.header, label %exit

loop.header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop.header ]
  call hotspotcc void @jeandle.safepoint_poll()
  %iv.next = add nsw i64 %iv, 1
  %exit.cond = icmp slt i64 %iv.next, %n
  br i1 %exit.cond, label %loop.header, label %exit

exit:
  ret void
}

!java-method-compilation = !{}

; CHECK-LABEL: @param_bound(
; CHECK: call hotspotcc void @jeandle.safepoint_poll()
