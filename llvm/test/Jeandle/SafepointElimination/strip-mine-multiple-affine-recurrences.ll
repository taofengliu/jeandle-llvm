; RUN: opt -passes='safepoint-elimination<early>,safepoint-elimination<strip-mining>' -jeandle-enable-strip-mining -S < %s | FileCheck %s
; RUN: opt -passes='safepoint-elimination<early>,safepoint-elimination<strip-mining>,verify<jeandle-safepoint-coverage>' \
; RUN:   -jeandle-enable-strip-mining -jeandle-verify-safepoint-coverage=fatal \
; RUN:   -disable-output < %s

; Two affine recurrences in the header (%i step 1, %j step 2). The exit test is
; driven by %j, so strip mining must pick %j as the IV — not whichever header
; phi comes first. The batch advance is N*|step(%j)| = 1000*2 = 2000, and %i is
; lifted through the outer loop as a secondary recurrence.

declare hotspotcc void @jeandle.safepoint_poll()

define i64 @f(i64 %n) {
entry:
  br label %header

header:
  %i = phi i64 [ 0, %entry ], [ %i.next, %latch ]
  %j = phi i64 [ 5, %entry ], [ %j.next, %latch ]
  %cond = icmp slt i64 %j, %n
  br i1 %cond, label %body, label %exit

body:
  %i.next = add i64 %i, 1
  ; The non-unit exit recurrence needs an explicit nowrap fact; otherwise a
  ; symbolic limit could let the IV wrap inside a poll-free batch.
  %j.next = add nsw i64 %j, 2
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %i.next, i64 %j.next) ]
  br label %latch

latch:
  br label %header

exit:
  %i.lcssa = phi i64 [ %i, %header ]
  ret i64 %i.lcssa
}

!java-method-compilation = !{}

; CHECK-LABEL: @f(
; %i is lifted as a secondary recurrence through the outer loop.
; CHECK:         %i.outer = phi i64
; The exit drives %j: the outer condition and clamp are on %j's recurrence
; (step 2 -> batch advance of 2000).
; CHECK:         %outer.cond = icmp slt i64 %outer.iv, %n
; CHECK:         %outer.batch.end = call i64 @llvm.sadd.sat.i64(i64 %outer.iv, i64 2000)
; CHECK:         call hotspotcc void @jeandle.safepoint_poll(){{.*}}!poll-coverage
