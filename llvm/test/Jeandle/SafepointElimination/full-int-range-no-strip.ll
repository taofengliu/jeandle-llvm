; RUN: opt -verify-each -jeandle-loop-strip-mining-iter=0 -passes="safepoint-poll-elimination,verify<jeandle-safepoint-coverage>" -S < %s | FileCheck %s
; RUN: opt -verify-each -jeandle-loop-strip-mining-iter=1 -passes="safepoint-poll-elimination<after-strip-mining>,verify<jeandle-safepoint-coverage>" -S < %s | FileCheck %s --check-prefix=PER-ITER
; CHECK-LABEL: define void @whole_int_range(
; CHECK-NOT: call hotspotcc void @jeandle.safepoint_poll
; CHECK: ret void
; PER-ITER-LABEL: define void @whole_int_range(
; PER-ITER: call hotspotcc void @jeandle.safepoint_poll
; PER-ITER: ret void

declare hotspotcc void @jeandle.safepoint_poll()
declare hotspotcc void @java_method()

define void @whole_int_range(ptr %sink) "java-method" {
entry: br label %loop
loop:
  %i = phi i32 [-2147483648, %entry], [%next, %loop]
  store volatile i32 %i, ptr %sink
  %next = add nsw i32 %i, 1
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 %next)]
  %again = icmp slt i32 %next, 2147483647
  br i1 %again, label %loop, label %exit
exit: ret void
}
!java-method-compilation = !{}

; A widened numerical bound uses the same policy. Equality at the maximum
; backedge threshold is rejected, without truncating it into the IV type.
define void @widened_threshold(ptr %sink) "java-method" {
entry: br label %loop
loop:
  %i = phi i64 [0, %entry], [%next, %loop]
  store volatile i64 %i, ptr %sink
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i64 %i)]
  %next = add nuw nsw i64 %i, 1
  %again = icmp ult i64 %next, 4294967296
  br i1 %again, label %loop, label %exit
exit: ret void
}
; CHECK-LABEL: define void @widened_threshold(
; CHECK: call hotspotcc void @jeandle.safepoint_poll()
; CHECK: ret void
; PER-ITER-LABEL: define void @widened_threshold(
; PER-ITER: call hotspotcc void @jeandle.safepoint_poll()
; PER-ITER: ret void

; The widened comparison cannot prove the actual modular i32 update safe.
define void @widened_wrapping_root(ptr %sink) "java-method" {
entry: br label %loop
loop:
  %i = phi i32 [2147483646, %entry], [%next, %body]
  %wide = sext i32 %i to i64
  %keep = icmp slt i64 %wide, 2147483648
  br i1 %keep, label %body, label %exit
body:
  store volatile i32 %i, ptr %sink
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 %i)]
  %next = add i32 %i, 1
  br label %loop
exit: ret void
}
; CHECK-LABEL: define void @widened_wrapping_root(
; CHECK: call hotspotcc void @jeandle.safepoint_poll()
; CHECK: ret void
; PER-ITER-LABEL: define void @widened_wrapping_root(
; PER-ITER: call hotspotcc void @jeandle.safepoint_poll()
; PER-ITER: ret void

; The widened comparison cannot prove the actual modular i32 update safe.
define void @widened_nonunit_wrap(ptr %sink) "java-method" {
entry: br label %loop
loop:
  %i = phi i32 [0, %entry], [%next, %body]
  %wide = sext i32 %i to i64
  %keep = icmp slt i64 %wide, 2147483647
  br i1 %keep, label %body, label %exit
body:
  store volatile i32 %i, ptr %sink
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 %i)]
  %next = add i32 %i, 2
  br label %loop
exit: ret void
}
; CHECK-LABEL: define void @widened_nonunit_wrap(
; CHECK: call hotspotcc void @jeandle.safepoint_poll()
; CHECK: ret void
; PER-ITER-LABEL: define void @widened_nonunit_wrap(
; PER-ITER: call hotspotcc void @jeandle.safepoint_poll()
; PER-ITER: ret void
