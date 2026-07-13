; RUN: opt -passes='safepoint-elimination<early>,safepoint-elimination<strip-mining>,verify<jeandle-safepoint-coverage>' \
; RUN:   -jeandle-enable-strip-mining -jeandle-verify-safepoint-coverage=fatal \
; RUN:   -S < %s | FileCheck %s

; Inclusive non-unit loops are safe to strip-mine when the final latch step
; after the last executed IV is proven not to wrap. These fixtures mirror the
; step-2 counted-loop shapes that appear in C2 strip-mining regression tests,
; but keep the proof local to IR: the real limit is statically away from the
; signed type extreme.

declare hotspotcc void @jeandle.safepoint_poll()

define void @inc_step2_masked_limit(i64 noundef %raw) {
entry:
  %n = and i64 %raw, 1000000
  br label %header

header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %latch ]
  %cond = icmp sle i64 %iv, %n
  br i1 %cond, label %body, label %exit

body:
  %iv.next = add i64 %iv, 2
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
  br label %latch

latch:
  br label %header

exit:
  ret void
}

; CHECK-LABEL: @inc_step2_masked_limit(
; CHECK:         %outer.cond = icmp sle i64 %outer.iv, %n
; CHECK:         %outer.batch.end = call i64 @llvm.sadd.sat.i64(i64 %outer.iv, i64 1998)
; CHECK:         icmp sle i64 %outer.batch.end, %n
; CHECK:         call hotspotcc void @jeandle.safepoint_poll(){{.*}}!poll-coverage

define void @dec_step2_to_zero(i64 noundef %raw) {
entry:
  %start = and i64 %raw, 1000000
  br label %header

header:
  %iv = phi i64 [ %start, %entry ], [ %iv.next, %latch ]
  %cond = icmp sge i64 %iv, 0
  br i1 %cond, label %body, label %exit

body:
  %iv.next = add i64 %iv, -2
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
  br label %latch

latch:
  br label %header

exit:
  ret void
}

; CHECK-LABEL: @dec_step2_to_zero(
; CHECK:         %outer.cond = icmp sge i64 %outer.iv, 0
; CHECK:         %outer.batch.end = call i64 @llvm.ssub.sat.i64(i64 %outer.iv, i64 1998)
; CHECK:         icmp sge i64 %outer.batch.end, 0
; CHECK:         call hotspotcc void @jeandle.safepoint_poll(){{.*}}!poll-coverage

define void @uinc_step2_masked_limit(i64 noundef %raw) {
entry:
  %n = and i64 %raw, 1000000
  br label %header

header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %latch ]
  %cond = icmp ule i64 %iv, %n
  br i1 %cond, label %body, label %exit

body:
  %iv.next = add i64 %iv, 2
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
  br label %latch

latch:
  br label %header

exit:
  ret void
}

; CHECK-LABEL: @uinc_step2_masked_limit(
; CHECK:         %outer.cond = icmp ule i64 %outer.iv, %n
; CHECK:         %outer.batch.end = call i64 @llvm.uadd.sat.i64(i64 %outer.iv, i64 1998)
; CHECK:         icmp ule i64 %outer.batch.end, %n
; CHECK:         call hotspotcc void @jeandle.safepoint_poll(){{.*}}!poll-coverage

define void @udec_step2_to_two(i64 noundef %raw) {
entry:
  br label %header

header:
  %iv = phi i64 [ %raw, %entry ], [ %iv.next, %latch ]
  %cond = icmp uge i64 %iv, 2
  br i1 %cond, label %body, label %exit

body:
  %iv.next = add i64 %iv, -2
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
  br label %latch

latch:
  br label %header

exit:
  ret void
}

; CHECK-LABEL: @udec_step2_to_two(
; CHECK:         %outer.cond = icmp uge i64 %outer.iv, 2
; CHECK:         %outer.batch.end = call i64 @llvm.usub.sat.i64(i64 %outer.iv, i64 1998)
; CHECK:         icmp uge i64 %outer.batch.end, 2
; CHECK:         call hotspotcc void @jeandle.safepoint_poll(){{.*}}!poll-coverage

!java-method-compilation = !{}
