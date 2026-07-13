; RUN: opt -passes='safepoint-elimination<early>,safepoint-elimination<strip-mining>,verify<jeandle-safepoint-coverage>' \
; RUN:   -jeandle-enable-strip-mining -jeandle-verify-safepoint-coverage=fatal \
; RUN:   -S < %s | FileCheck %s

; C2 treats `i != limit` as a counted loop only for unit strides and a proven
; start/limit order. The strip-mined outer loop uses the equivalent signed
; relational predicate while the inner exit keeps the original equality form.

declare hotspotcc void @jeandle.safepoint_poll()

define void @header_ne_inc(i64 %n) {
entry:
  %entry.guard = icmp sle i64 0, %n
  br i1 %entry.guard, label %loop.preheader, label %return

loop.preheader:
  br label %header

header:
  %iv = phi i64 [ 0, %loop.preheader ], [ %iv.next, %latch ]
  %cond = icmp ne i64 %iv, %n
  br i1 %cond, label %body, label %loop.exit

body:
  %iv.next = add nsw i64 %iv, 1
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
  br label %latch

latch:
  br label %header

loop.exit:
  br label %return

return:
  ret void
}

; CHECK-LABEL: @header_ne_inc(
; CHECK:       header:
; CHECK:         %cond = icmp ne i64 %iv, %outer.inner.limit
; CHECK:         br i1 %cond, label %body, label %header.outer.latch
; CHECK:       latch:
; CHECK:         br label %header, !strip-mined
; CHECK:       header.outer:
; CHECK:         %outer.cond = icmp slt i64 %outer.iv, %n

define void @header_eq_exit_on_true(i64 %n) {
entry:
  %entry.guard = icmp sle i64 0, %n
  br i1 %entry.guard, label %loop.preheader, label %return

loop.preheader:
  br label %header

header:
  %iv = phi i64 [ 0, %loop.preheader ], [ %iv.next, %latch ]
  %done = icmp eq i64 %iv, %n
  br i1 %done, label %loop.exit, label %body

body:
  %iv.next = add nsw i64 %iv, 1
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
  br label %latch

latch:
  br label %header

loop.exit:
  br label %return

return:
  ret void
}

; CHECK-LABEL: @header_eq_exit_on_true(
; CHECK:       header:
; CHECK:         %done = icmp eq i64 %iv, %outer.inner.limit
; CHECK:         br i1 %done, label %header.outer.latch, label %body
; CHECK:       latch:
; CHECK:         br label %header, !strip-mined
; CHECK:       header.outer:
; CHECK:         %outer.cond = icmp slt i64 %outer.iv, %n

define void @latch_ne_inc(i64 %n) {
entry:
  %entry.guard = icmp slt i64 0, %n
  br i1 %entry.guard, label %loop.preheader, label %return

loop.preheader:
  br label %header

header:
  %iv = phi i64 [ 0, %loop.preheader ], [ %iv.next, %latch ]
  br label %latch

latch:
  %iv.next = add nsw i64 %iv, 1
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
  %cond = icmp ne i64 %iv.next, %n
  br i1 %cond, label %header, label %loop.exit

loop.exit:
  br label %return

return:
  ret void
}

; CHECK-LABEL: @latch_ne_inc(
; CHECK:       latch:
; CHECK-NOT:     call hotspotcc void @jeandle.safepoint_poll
; CHECK:         %cond = icmp ne i64 %iv.next, %outer.inner.limit
; CHECK:         br i1 %cond, label %header, label %header.outer.latch, !strip-mined
; CHECK:       header.outer:
; CHECK:         %outer.cond = icmp slt i64 %outer.iv, %n

define void @header_ne_dec(i64 %n) {
entry:
  %entry.guard = icmp sge i64 10, %n
  br i1 %entry.guard, label %loop.preheader, label %return

loop.preheader:
  br label %header

header:
  %iv = phi i64 [ 10, %loop.preheader ], [ %iv.next, %latch ]
  %cond = icmp ne i64 %iv, %n
  br i1 %cond, label %body, label %loop.exit

body:
  %iv.next = add nsw i64 %iv, -1
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
  br label %latch

latch:
  br label %header

loop.exit:
  br label %return

return:
  ret void
}

; CHECK-LABEL: @header_ne_dec(
; CHECK:       header:
; CHECK:         %cond = icmp ne i64 %iv, %outer.inner.limit
; CHECK:       header.outer:
; CHECK:         %outer.cond = icmp sgt i64 %outer.iv, %n
; CHECK:         %outer.batch.end = call i64 @llvm.ssub.sat.i64(i64 %outer.iv, i64 1000)

define void @header_ne_swapped(i64 %n) {
entry:
  %entry.guard = icmp sle i64 0, %n
  br i1 %entry.guard, label %loop.preheader, label %return

loop.preheader:
  br label %header

header:
  %iv = phi i64 [ 0, %loop.preheader ], [ %iv.next, %latch ]
  %cond = icmp ne i64 %n, %iv
  br i1 %cond, label %body, label %loop.exit

body:
  %iv.next = add nsw i64 %iv, 1
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
  br label %latch

latch:
  br label %header

loop.exit:
  br label %return

return:
  ret void
}

; CHECK-LABEL: @header_ne_swapped(
; CHECK:       header:
; CHECK:         %cond = icmp ne i64 %outer.inner.limit, %iv
; CHECK:       header.outer:
; CHECK:         %outer.cond = icmp slt i64 %outer.iv, %n

define void @latch_ne_dec(i64 %n) {
entry:
  %entry.guard = icmp sgt i64 10, %n
  br i1 %entry.guard, label %loop.preheader, label %return

loop.preheader:
  br label %header

header:
  %iv = phi i64 [ 10, %loop.preheader ], [ %iv.next, %latch ]
  br label %latch

latch:
  %iv.next = add nsw i64 %iv, -1
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
  %cond = icmp ne i64 %iv.next, %n
  br i1 %cond, label %header, label %loop.exit

loop.exit:
  br label %return

return:
  ret void
}

; CHECK-LABEL: @latch_ne_dec(
; CHECK:       latch:
; CHECK-NOT:     call hotspotcc void @jeandle.safepoint_poll
; CHECK:         %cond = icmp ne i64 %iv.next, %outer.inner.limit
; CHECK:         br i1 %cond, label %header, label %header.outer.latch, !strip-mined
; CHECK:       header.outer:
; CHECK:         %outer.cond = icmp sgt i64 %outer.iv, %n
; CHECK:         %outer.batch.end = call i64 @llvm.ssub.sat.i64(i64 %outer.iv, i64 1000)
!java-method-compilation = !{}
