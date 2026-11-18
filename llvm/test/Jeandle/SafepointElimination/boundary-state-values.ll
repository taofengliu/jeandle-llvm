; RUN: opt -passes='loop-simplify,lcssa,safepoint-strip-mining<strip-mining>,safepoint-poll-elimination<after-strip-mining>,verify<jeandle-safepoint-coverage>' -jeandle-loop-strip-mining-iter=4 -jeandle-verify-safepoint-coverage=fatal -verify-each -S %s | FileCheck %s

; Keep exact current, next and derived SSA values from the last original poll.
; They need not all be header recurrence next values.
declare hotspotcc void @jeandle.safepoint_poll()

define void @boundary_state(ptr %out, i64 noundef %n) "java-method" {
entry:
  %positive = icmp sgt i64 %n, 0
  br i1 %positive, label %preheader, label %exit
preheader:
  br label %loop
loop:
  %iv = phi i64 [0, %preheader], [%next, %loop]
  %a = phi i64 [1, %preheader], [%b, %loop]
  %b = phi i64 [2, %preheader], [%a, %loop]
  %derived = xor i64 %iv, 7
  store i64 %derived, ptr %out
  %next = add nsw i64 %iv, 1
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i64 %iv, i64 %next, i64 %derived, i64 %a, i64 %b)]
  %cond = icmp slt i64 %next, %n
  br i1 %cond, label %loop, label %exit
exit:
  ret void
}

; CHECK-LABEL: @boundary_state(
; CHECK: loop.outer.latch:
; CHECK-DAG: %[[CURRENT:[^ ]+]] = phi i64 [ %iv, %loop ]
; CHECK-DAG: %[[DERIVED:[^ ]+]] = phi i64 [ %derived, %loop ]
; CHECK: call hotspotcc void @jeandle.safepoint_poll() {{.*}}[ "deopt"(i64 %[[CURRENT]], i64 %outer.iv.next, i64 %[[DERIVED]], i64 %b.outer.next, i64 %a.outer.next) ]

!java-method-compilation = !{}
