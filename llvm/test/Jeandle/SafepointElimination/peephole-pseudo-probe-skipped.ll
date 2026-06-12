; RUN: opt -passes=safepoint-elimination -S < %s | FileCheck %s

; Two polls separated only by an `llvm.pseudoprobe` call, which exercises the
; `isDebugOrPseudoInst()` half of the adjacency-transparency test (lifetime
; markers cover the other half in peephole-lifetime-markers-skipped.ll). A
; pseudo probe has no observable side effect, so the polls collapse and the
; probe stays.

declare hotspotcc void @jeandle.safepoint_poll()
declare void @llvm.pseudoprobe(i64, i64, i32, i64)

define void @pseudoprobe_between_polls(i64 %n) gc "safepoint-in-loop-example" {
entry:
  %cmp = icmp sgt i64 %n, 0
  br i1 %cmp, label %loop.header, label %exit

loop.header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop.latch ]
  %exit.cond = icmp slt i64 %iv, %n
  br i1 %exit.cond, label %loop.body, label %exit

loop.body:
  call hotspotcc void @jeandle.safepoint_poll()
  call void @llvm.pseudoprobe(i64 0, i64 1, i32 0, i64 -1)
  call hotspotcc void @jeandle.safepoint_poll()
  br label %loop.latch

loop.latch:
  %iv.next = add nsw i64 %iv, 1
  br label %loop.header

exit:
  ret void
}

!java-method-compilation = !{}

; CHECK-LABEL: @pseudoprobe_between_polls(
; CHECK:       loop.body:
; CHECK-NEXT:    call void @llvm.pseudoprobe(i64 0, i64 1, i32 0, i64 -1)
; CHECK-NEXT:    call hotspotcc void @jeandle.safepoint_poll()
; CHECK-NEXT:    br label %loop.latch
