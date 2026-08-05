; RUN: opt -passes='loop-simplify,lcssa,loop-rotate,safepoint-poll-elimination<early>,safepoint-strip-mining<strip-mining>,safepoint-poll-elimination<after-strip-mining>' -S < %s | FileCheck %s
; RUN: opt -passes='loop-simplify,lcssa,loop-rotate,safepoint-poll-elimination<early>,safepoint-strip-mining<strip-mining>,safepoint-poll-elimination<after-strip-mining>,verify<jeandle-safepoint-coverage>' \
; RUN:   -jeandle-verify-safepoint-coverage=fatal \
; RUN:   -disable-output < %s

; Decreasing loop (step -1, `i > 0`). The batch clamp uses saturating
; subtraction toward the limit and the outer condition keeps the sgt direction.

declare hotspotcc void @jeandle.safepoint_poll()

define void @countdown(i64 %n) "java-method" {
entry:
  br label %header

header:
  %iv = phi i64 [ %n, %entry ], [ %iv.next, %latch ]
  %cond = icmp sgt i64 %iv, 0
  br i1 %cond, label %body, label %exit

body:
  %iv.next = add i64 %iv, -1
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
  br label %latch

latch:
  br label %header

exit:
  ret void
}

!java-method-compilation = !{}

; CHECK-LABEL: @countdown(
; CHECK:         %outer.cond = icmp sgt i64 %outer.iv, 0
; CHECK:         %outer.batch.end = call i64 @llvm.ssub.sat.i64(i64 %outer.iv, i64 1000)
; CHECK:         call hotspotcc void @jeandle.safepoint_poll()
