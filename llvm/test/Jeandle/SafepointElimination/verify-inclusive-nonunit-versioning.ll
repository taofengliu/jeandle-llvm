; RUN: opt -passes='verify<jeandle-safepoint-coverage>' \
; RUN:   -jeandle-safepoint-chunk-iters=8 \
; RUN:   -jeandle-verify-safepoint-coverage=warn -S < %s 2>&1 \
; RUN:   | FileCheck %s
; RUN: not --crash opt -passes='verify<jeandle-safepoint-coverage>' \
; RUN:   -jeandle-safepoint-chunk-iters=8 \
; RUN:   -jeandle-verify-safepoint-coverage=fatal -disable-output < %s 2>&1 \
; RUN:   | FileCheck %s --check-prefix=ABORT

; A transform marker and an outer poll are not proof. These step-3 loops use
; unit-stride no-wrap guards, which do not keep the real inclusive limit far
; enough from the signed edge for the final update.

declare hotspotcc void @jeandle.safepoint_poll()
declare i32 @llvm.sadd.sat.i32(i32, i32)
declare i32 @llvm.ssub.sat.i32(i32, i32)

define void @forged_increasing_step3_unit_margin(i32 noundef %start,
                                                 i32 noundef %limit) {
entry:
  %wrong.margin = icmp slt i32 %limit, 2147483647
  br i1 %wrong.margin, label %loop.outer, label %exit

loop.outer:
  %outer.iv = phi i32 [ %start, %entry ], [ %outer.iv.next, %loop.outer.latch ]
  %outer.cond = icmp sle i32 %outer.iv, %limit
  br i1 %outer.cond, label %loop.outer.inner.entry, label %exit

loop.outer.inner.entry:
  %outer.batch.end = call i32 @llvm.sadd.sat.i32(i32 %outer.iv, i32 21)
  %outer.cap.cond = icmp sle i32 %outer.batch.end, %limit
  %outer.inner.limit = select i1 %outer.cap.cond, i32 %outer.batch.end, i32 %limit
  br label %loop

loop:
  %iv = phi i32 [ %outer.iv, %loop.outer.inner.entry ], [ %iv.next, %loop ]
  %iv.next = add i32 %iv, 3
  %done = icmp sgt i32 %iv.next, %outer.inner.limit
  br i1 %done, label %loop.outer.latch, label %loop, !strip-mined !0

loop.outer.latch:
  %outer.iv.next = phi i32 [ %iv.next, %loop ]
  call hotspotcc void @jeandle.safepoint_poll(), !poll-coverage !0
  br label %loop.outer

exit:
  ret void
}

define void @forged_decreasing_step3_unit_margin(i32 noundef %start,
                                                 i32 noundef %limit) {
entry:
  %wrong.margin = icmp sgt i32 %limit, -2147483648
  br i1 %wrong.margin, label %loop.outer, label %exit

loop.outer:
  %outer.iv = phi i32 [ %start, %entry ], [ %outer.iv.next, %loop.outer.latch ]
  %outer.cond = icmp sge i32 %outer.iv, %limit
  br i1 %outer.cond, label %loop.outer.inner.entry, label %exit

loop.outer.inner.entry:
  %outer.batch.end = call i32 @llvm.ssub.sat.i32(i32 %outer.iv, i32 21)
  %outer.cap.cond = icmp sge i32 %outer.batch.end, %limit
  %outer.inner.limit = select i1 %outer.cap.cond, i32 %outer.batch.end, i32 %limit
  br label %loop

loop:
  %iv = phi i32 [ %outer.iv, %loop.outer.inner.entry ], [ %iv.next, %loop ]
  %iv.next = add i32 %iv, -3
  %done = icmp slt i32 %iv.next, %outer.inner.limit
  br i1 %done, label %loop.outer.latch, label %loop, !strip-mined !0

loop.outer.latch:
  %outer.iv.next = phi i32 [ %iv.next, %loop ]
  call hotspotcc void @jeandle.safepoint_poll(), !poll-coverage !0
  br label %loop.outer

exit:
  ret void
}

!java-method-compilation = !{}
!0 = !{}

; CHECK: SafepointCoverageVerifier: loop with header 'loop' in function 'forged_increasing_step3_unit_margin' has no dominating safepoint poll and no provable trip bound
; CHECK: SafepointCoverageVerifier: loop with header 'loop' in function 'forged_decreasing_step3_unit_margin' has no dominating safepoint poll and no provable trip bound
; ABORT: SafepointCoverageVerifier: loop with header 'loop' in function 'forged_increasing_step3_unit_margin' has no dominating safepoint poll and no provable trip bound
; ABORT: LLVM ERROR: Jeandle safepoint coverage verification failed
