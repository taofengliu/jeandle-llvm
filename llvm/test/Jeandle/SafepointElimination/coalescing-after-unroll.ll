; RUN: opt -verify-each -passes='loop-simplify,lcssa,loop-unroll' -S %s -o %t.unrolled.ll
; RUN: FileCheck %s --check-prefix=INPUT < %t.unrolled.ll
; RUN: opt -jeandle-enable-branch-poll-coalescing -verify-each -passes='safepoint-poll-elimination,verify<jeandle-safepoint-coverage>' -S %t.unrolled.ll | FileCheck %s --check-prefix=OUTPUT
; RUN: opt -jeandle-enable-branch-poll-coalescing -verify-each -passes='safepoint-poll-elimination,safepoint-poll-elimination' -S %t.unrolled.ll | FileCheck %s --check-prefix=OUTPUT

; Full unrolling removes the loop but leaves distinct BCI polls in both arms
; of every iteration. Check the actual input to poll elimination, not just
; that a final output with no loop happens to contain fewer polls.
declare hotspotcc void @jeandle.safepoint_poll()

define void @unrolled_diamonds(ptr %flag, ptr %output) "java-method" {
entry:
  br label %loop
loop:
  %i = phi i32 [0, %entry], [%next, %latch]
  %choose = load volatile i1, ptr %flag
  br i1 %choose, label %left, label %right
left:
  store i32 %i, ptr %output
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 10, i32 %i)]
  br label %latch
right:
  store i32 17, ptr %output
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 20, i32 %i)]
  br label %latch
latch:
  %next = add nuw nsw i32 %i, 1
  %continue = icmp ult i32 %next, 3
  br i1 %continue, label %loop, label %exit, !llvm.loop !0
exit:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 30)]
  ret void
}

; INPUT-LABEL: define void @unrolled_diamonds(
; INPUT-COUNT-7: call hotspotcc void @jeandle.safepoint_poll()
; INPUT-NOT: call hotspotcc void @jeandle.safepoint_poll()
; INPUT: ret void
; OUTPUT-LABEL: define void @unrolled_diamonds(
; OUTPUT-NOT: call hotspotcc void @jeandle.safepoint_poll()
; OUTPUT: call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i32 30) ]
; OUTPUT-NEXT: ret void

!java-method-compilation = !{}
!0 = distinct !{!0, !1}
!1 = !{!"llvm.loop.unroll.full"}
