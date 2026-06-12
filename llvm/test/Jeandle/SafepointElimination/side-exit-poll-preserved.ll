; RUN: opt -passes=safepoint-elimination -S < %s | FileCheck %s

; A poll on a side-exit path (a block with no path back to the loop, carrying
; deopt state for an uncommon trap) is outside the loop's blocks and must
; never enter any candidate set: here the loop itself is provably short, so
; its own poll is deleted, but the side-exit poll survives untouched.

declare hotspotcc void @jeandle.safepoint_poll()
declare void @uncommon_trap()

define void @side_exit(ptr %a, i1 %deopt) gc "safepoint-in-loop-example" {
entry:
  br label %loop.header

loop.header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop.latch ]
  call hotspotcc void @jeandle.safepoint_poll()
  br i1 %deopt, label %side.exit, label %loop.latch

loop.latch:
  %p = getelementptr inbounds i32, ptr %a, i64 %iv
  store i32 0, ptr %p
  %iv.next = add nuw nsw i64 %iv, 1
  %exit.cond = icmp slt i64 %iv.next, 100
  br i1 %exit.cond, label %loop.header, label %exit

side.exit:
  call hotspotcc void @jeandle.safepoint_poll()
  call void @uncommon_trap()
  unreachable

exit:
  ret void
}

!java-method-compilation = !{}

; CHECK-LABEL: @side_exit(
; CHECK:       loop.header:
; CHECK-NOT:     call hotspotcc void @jeandle.safepoint_poll
; CHECK:       side.exit:
; CHECK-NEXT:    call hotspotcc void @jeandle.safepoint_poll()
; CHECK-NEXT:    call void @uncommon_trap()
