; RUN: opt -passes=safepoint-elimination -S < %s | FileCheck %s

; Two polls separated only by lifetime markers, which exercise the
; `isLifetimeStartOrEnd()` half of the adjacency-transparency test. The polls
; still collapse. The `isDebugOrPseudoInst()` half is covered by
; peephole-pseudo-probe-skipped.ll.

declare hotspotcc void @jeandle.safepoint_poll()
declare void @llvm.lifetime.start.p0(ptr captures(none))
declare void @llvm.lifetime.end.p0(ptr captures(none))

define void @lifetime_between_polls(i64 %n) gc "safepoint-in-loop-example" {
entry:
  %slot = alloca i8, align 1
  %cmp = icmp sgt i64 %n, 0
  br i1 %cmp, label %loop.header, label %exit

loop.header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop.latch ]
  %exit.cond = icmp slt i64 %iv, %n
  br i1 %exit.cond, label %loop.body, label %exit

loop.body:
  call hotspotcc void @jeandle.safepoint_poll()
  call void @llvm.lifetime.start.p0(ptr %slot)
  call void @llvm.lifetime.end.p0(ptr %slot)
  call hotspotcc void @jeandle.safepoint_poll()
  br label %loop.latch

loop.latch:
  %iv.next = add nsw i64 %iv, 1
  br label %loop.header

exit:
  ret void
}

!java-method-compilation = !{}

; CHECK-LABEL: @lifetime_between_polls(
; CHECK:       loop.body:
; CHECK-NEXT:    call void @llvm.lifetime.start.p0
; CHECK-NEXT:    call void @llvm.lifetime.end.p0
; CHECK-NEXT:    call hotspotcc void @jeandle.safepoint_poll()
; CHECK-NEXT:    br label %loop.latch
