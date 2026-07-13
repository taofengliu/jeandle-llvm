; RUN: opt -passes='safepoint-elimination<early>,safepoint-elimination<strip-mining>' -jeandle-enable-strip-mining -S < %s | FileCheck %s

; Latch-exit support is limited to guarded post-increment exits. An unguarded
; do-while shape would lose its first iteration if wrapped by a pre-tested outer
; loop, and an old-IV latch compare has different batch-boundary semantics.

declare hotspotcc void @jeandle.safepoint_poll()

define void @unguarded_latch_next(i64 %n) {
entry:
  br label %header

header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %latch ]
  br label %latch

latch:
  %iv.next = add nuw nsw i64 %iv, 1
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
  %cond = icmp slt i64 %iv.next, %n
  br i1 %cond, label %header, label %exit

exit:
  ret void
}

; CHECK-LABEL: @unguarded_latch_next(
; CHECK-NOT:   .outer
; CHECK:       call hotspotcc void @jeandle.safepoint_poll()
; CHECK-NOT:   !strip-mined


define void @latch_old_iv(i64 %n) {
entry:
  %entry.guard = icmp slt i64 0, %n
  br i1 %entry.guard, label %loop.preheader, label %return

loop.preheader:
  br label %header

header:
  %iv = phi i64 [ 0, %loop.preheader ], [ %iv.next, %latch ]
  br label %latch

latch:
  %iv.next = add nuw nsw i64 %iv, 1
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
  %cond = icmp slt i64 %iv, %n
  br i1 %cond, label %header, label %loop.exit

loop.exit:
  br label %return

return:
  ret void
}

; CHECK-LABEL: @latch_old_iv(
; CHECK-NOT:   .outer
; CHECK:       call hotspotcc void @jeandle.safepoint_poll()
; CHECK-NOT:   !strip-mined

define void @latch_next_sub_form(i64 %n) {
entry:
  %entry.guard = icmp slt i64 0, %n
  br i1 %entry.guard, label %loop.preheader, label %return

loop.preheader:
  br label %header

header:
  %iv = phi i64 [ 0, %loop.preheader ], [ %iv.next, %latch ]
  br label %latch

latch:
  %iv.next = sub nuw nsw i64 %iv, -1
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
  %cond = icmp slt i64 %iv.next, %n
  br i1 %cond, label %header, label %loop.exit

loop.exit:
  br label %return

return:
  ret void
}

; CHECK-LABEL: @latch_next_sub_form(
; CHECK-NOT:   .outer
; CHECK:       call hotspotcc void @jeandle.safepoint_poll()
; CHECK-NOT:   !strip-mined

!java-method-compilation = !{}
