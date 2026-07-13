; RUN: opt -passes='safepoint-elimination,verify<jeandle-safepoint-coverage>' \
; RUN:   -jeandle-verify-safepoint-coverage=fatal -S < %s 2>&1 | FileCheck %s

; All loops satisfy the coverage invariant after elimination: the first keeps
; a dominating poll (parameter bound), the second loses its poll under the
; trip-count proof, and the multi-latch loop covers each backedge separately.
; The verifier must stay silent.

declare hotspotcc void @jeandle.safepoint_poll()

define void @covered_by_poll(i64 %n) gc "safepoint-in-loop-example" {
entry:
  br label %loop.header

loop.header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop.header ]
  call hotspotcc void @jeandle.safepoint_poll()
  %iv.next = add nsw i64 %iv, 1
  %exit.cond = icmp slt i64 %iv.next, %n
  br i1 %exit.cond, label %loop.header, label %exit

exit:
  ret void
}

define void @covered_by_bound(ptr %a) gc "safepoint-in-loop-example" {
entry:
  br label %loop.header

loop.header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop.header ]
  call hotspotcc void @jeandle.safepoint_poll()
  %p = getelementptr inbounds i32, ptr %a, i64 %iv
  store i32 0, ptr %p
  %iv.next = add nuw nsw i64 %iv, 1
  %exit.cond = icmp slt i64 %iv.next, 100
  br i1 %exit.cond, label %loop.header, label %exit

exit:
  ret void
}

define void @multi_latch_covered(i64 %n, i1 %choose) gc "safepoint-in-loop-example" {
entry:
  br label %loop.header

loop.header:
  %iv = phi i64 [ 0, %entry ], [ %iv.a, %latch.a ], [ %iv.b, %latch.b ]
  %exit.cond = icmp slt i64 %iv, %n
  br i1 %exit.cond, label %body, label %exit

body:
  br i1 %choose, label %latch.a, label %latch.b

latch.a:
  %iv.a = add i64 %iv, 1
  call hotspotcc void @jeandle.safepoint_poll()
  br label %loop.header

latch.b:
  %iv.b = add i64 %iv, 2
  call hotspotcc void @jeandle.safepoint_poll()
  br label %loop.header

exit:
  ret void
}

!java-method-compilation = !{}

; CHECK-NOT: SafepointCoverageVerifier:
; CHECK-LABEL: @covered_by_poll(
; CHECK: call hotspotcc void @jeandle.safepoint_poll()
; CHECK-LABEL: @covered_by_bound(
; CHECK-LABEL: @multi_latch_covered(
; CHECK-COUNT-2: call hotspotcc void @jeandle.safepoint_poll()
; CHECK-NOT: SafepointCoverageVerifier:
