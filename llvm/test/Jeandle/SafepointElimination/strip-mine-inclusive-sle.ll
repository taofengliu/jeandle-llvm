; RUN: opt -passes='safepoint-elimination<early>,safepoint-elimination<strip-mining>' -jeandle-enable-strip-mining -S < %s | FileCheck %s
; RUN: opt -passes='safepoint-elimination<early>,safepoint-elimination<strip-mining>,verify<jeandle-safepoint-coverage>' \
; RUN:   -jeandle-enable-strip-mining -jeandle-verify-safepoint-coverage=fatal \
; RUN:   -disable-output < %s

; Inclusive exit predicate (i <= n). `i <= batch_end` runs one more iteration
; than `i < batch_end`, so the per-batch advance is N-1 steps, keeping each
; batch within the N-iteration budget. With N=1000 and step 1 the batch end is
; outer.iv + 999.
;
; The limit must be provably below the type maximum: for `i <= n`, if n could be
; INT64_MAX the saturating batch-end pins the inner limit to INT64_MAX and the
; inner test `i <= INT64_MAX` never fails -- a poll-free infinite loop. Here the
; mask proves 0 <= n <= 1000000, so mining is safe. (The unbounded-parameter
; form is exercised as a bail in strip-mine-inclusive-extreme-bail.ll.)

declare hotspotcc void @jeandle.safepoint_poll()

define void @loop(i64 noundef %raw) {
entry:
  %n = and i64 %raw, 1000000
  br label %header

header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %latch ]
  %cond = icmp sle i64 %iv, %n
  br i1 %cond, label %body, label %exit

body:
  %iv.next = add i64 %iv, 1
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
  br label %latch

latch:
  br label %header

exit:
  ret void
}

!java-method-compilation = !{}

; CHECK-LABEL: @loop(
; CHECK:         %outer.cond = icmp sle i64 %outer.iv, %n
; CHECK:         %outer.batch.end = call i64 @llvm.sadd.sat.i64(i64 %outer.iv, i64 999)
; CHECK:         icmp sle i64 %outer.batch.end, %n
; CHECK:         call hotspotcc void @jeandle.safepoint_poll(){{.*}}!poll-coverage
