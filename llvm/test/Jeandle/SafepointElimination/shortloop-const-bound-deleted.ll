; RUN: opt -passes=safepoint-elimination -S < %s | FileCheck %s

; Innermost loop with a provable constant trip count (100) within the chunk
; budget (default 1000): the poll is deleted outright.

declare hotspotcc void @jeandle.safepoint_poll()

define void @short_const(ptr %a) gc "safepoint-in-loop-example" {
entry:
  br label %loop.header

loop.header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop.header ]
  call hotspotcc void @jeandle.safepoint_poll()
  %p = getelementptr inbounds i32, ptr %a, i64 %iv
  store i32 0, ptr %p
  %iv.next = add nuw nsw i64 %iv, 1
  %exit.cond = icmp slt i64 %iv.next, 100
  br i1 %exit.cond, label %loop.header, label %exit

exit:
  ret void
}

!java-method-compilation = !{}

; CHECK-LABEL: @short_const(
; CHECK-NOT: call hotspotcc void @jeandle.safepoint_poll
