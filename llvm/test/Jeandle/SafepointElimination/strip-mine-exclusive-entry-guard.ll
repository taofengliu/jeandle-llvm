; RUN: opt -passes='safepoint-elimination<early>,safepoint-elimination<strip-mining>' -jeandle-enable-strip-mining \
; RUN:   -jeandle-safepoint-chunk-iters=10 -S < %s | FileCheck %s
; RUN: opt -passes='safepoint-elimination<early>,safepoint-elimination<strip-mining>,verify<jeandle-safepoint-coverage>' \
; RUN:   -jeandle-enable-strip-mining -jeandle-verify-safepoint-coverage=fatal \
; RUN:   -jeandle-safepoint-chunk-iters=10 -disable-output < %s

; Exclusive non-unit loops may use a dominating entry guard to prove the real
; limit is far enough from the type edge. Transform and verifier must both
; replay that same real-limit proof.

declare hotspotcc void @jeandle.safepoint_poll()

define void @exclusive_step2_entry_guard(i64 %n) {
entry:
  %guard = icmp slt i64 %n, 1000
  br i1 %guard, label %preheader, label %return

preheader:
  br label %header

header:
  %iv = phi i64 [ 0, %preheader ], [ %iv.next, %latch ]
  %cond = icmp slt i64 %iv, %n
  br i1 %cond, label %body, label %loop.exit

body:
  %iv.next = add i64 %iv, 2
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
  br label %latch

latch:
  br label %header

loop.exit:
  br label %return

return:
  ret void
}

; CHECK-LABEL: @exclusive_step2_entry_guard(
; CHECK:         .outer
; CHECK:         br label %header, !strip-mined
; CHECK:         call hotspotcc void @jeandle.safepoint_poll(){{.*}}!poll-coverage

define void @exclusive_step2_wrong_edge_guard_bails(i64 %n) {
entry:
  %guard = icmp slt i64 %n, 1000
  br i1 %guard, label %return, label %preheader

preheader:
  br label %header

header:
  %iv = phi i64 [ 0, %preheader ], [ %iv.next, %latch ]
  %cond = icmp slt i64 %iv, %n
  br i1 %cond, label %body, label %loop.exit

body:
  %iv.next = add i64 %iv, 2
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
  br label %latch

latch:
  br label %header

loop.exit:
  br label %return

return:
  ret void
}

; CHECK-LABEL: @exclusive_step2_wrong_edge_guard_bails(
; CHECK-NOT:     .outer
; CHECK:         call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
; CHECK-NOT:     !strip-mined

!java-method-compilation = !{}
