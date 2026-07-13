; RUN: opt -passes='safepoint-elimination<early>,safepoint-elimination<strip-mining>' -jeandle-enable-strip-mining -S < %s | FileCheck %s

; The canonical Java `for (int i = 0; i < n; i++)` shape: an i32 IV (Java `int`)
; with a symbolic trip count and no secondary recurrence. Exercises the i32
; stride arithmetic (sadd.sat.i32), the empty lifted-phi case (only the IV is
; threaded, no `.outer` reduction phi), and a deopt bundle whose bci operand is
; left untouched while the loop-carried IV is remapped to the batch boundary.

declare hotspotcc void @jeandle.safepoint_poll()

define void @count(i32 %n, ptr %a) {
entry:
  br label %header

header:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %latch ]
  %cond = icmp slt i32 %iv, %n
  br i1 %cond, label %body, label %exit

body:
  %p = getelementptr inbounds i32, ptr %a, i32 %iv
  store i32 %iv, ptr %p
  %iv.next = add i32 %iv, 1
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i32 5, i32 %iv.next) ]
  br label %latch

latch:
  br label %header

exit:
  ret void
}

!java-method-compilation = !{}

; CHECK-LABEL: @count(

; The inner exit tests against the per-batch clamp.
; CHECK:       header:
; CHECK:         icmp slt i32 %iv, %outer.inner.limit

; Inner body runs poll-free; the inner latch is tagged bounded-by-construction.
; CHECK:       body:
; CHECK-NOT:     call hotspotcc void @jeandle.safepoint_poll
; CHECK:         br label %latch
; CHECK:       latch:
; CHECK:         br label %header, !strip-mined

; Per-batch limit clamped with an i32 saturating add against the budget.
; CHECK:       header.outer.inner.entry:
; CHECK:         %outer.batch.end = call i32 @llvm.sadd.sat.i32(i32 %outer.iv, i32 1000)
; CHECK:         %outer.inner.limit = select i1 %outer.cap.cond, i32 %outer.batch.end, i32 %n

; Single relocated poll on the outer back-edge: the bci operand (5) is carried
; over verbatim and only the IV is remapped to the batch-boundary recurrence.
; CHECK:       header.outer.latch:
; CHECK:         call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i32 5, i32 %outer.iv.next) ]{{.*}}!poll-coverage
