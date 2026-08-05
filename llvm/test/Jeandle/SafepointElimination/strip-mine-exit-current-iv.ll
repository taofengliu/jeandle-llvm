; RUN: split-file %s %t
; RUN: opt -jeandle-loop-strip-mining-iter=4 \
; RUN:   -jeandle-verify-safepoint-coverage=fatal \
; RUN:   -passes='loop-simplify,lcssa,safepoint-poll-elimination<early>,safepoint-strip-mining<strip-mining>,safepoint-poll-elimination<after-strip-mining>,verify<jeandle-safepoint-coverage>,verify' \
; RUN:   -S < %t/without-lag.ll | FileCheck %s --check-prefix=WITHOUT-LAG
; RUN: opt -jeandle-loop-strip-mining-iter=4 \
; RUN:   -jeandle-verify-safepoint-coverage=fatal \
; RUN:   -passes='loop-simplify,lcssa,loop-rotate,loop-mssa(licm),indvars,safepoint-poll-elimination<early>,safepoint-strip-mining<inclusive-loop-versioning>,safepoint-strip-mining<strip-mining>,safepoint-poll-elimination<after-strip-mining>,verify<jeandle-safepoint-coverage>,verify' \
; RUN:   -S < %t/with-lag.ll | FileCheck %s --check-prefix=WITH-LAG

; An LCSSA phi use occurs on the latch-to-exit edge. The raw header IV is the
; current iteration's value there, while the latch-carried next IV is the
; resume value. A lag recurrence carries the current IV between outer batches
; and supplies the phase-correct value at the final outer-header exit.

;--- with-lag.ll

declare hotspotcc void @jeandle.safepoint_poll()
declare void @use(i32)

define i32 @current_iv_with_lag(i32 noundef %n) "java-method" {
entry:
  %entry.guard = icmp slt i32 0, %n
  br i1 %entry.guard, label %loop.preheader, label %return

loop.preheader:
  br label %header

header:
  %iv = phi i32 [ 0, %loop.preheader ], [ %iv.next, %latch ]
  %last = phi i32 [ -1, %loop.preheader ], [ %iv, %latch ]
  br label %latch

latch:
  call void @use(i32 %last)
  %iv.next = add nuw nsw i32 %iv, 1
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i32 %iv.next) ]
  %cond = icmp slt i32 %iv.next, %n
  br i1 %cond, label %header, label %loop.exit

loop.exit:
  %last.lcssa = phi i32 [ %iv, %latch ]
  br label %return

return:
  %result = phi i32 [ -1, %entry ], [ %last.lcssa, %loop.exit ]
  ret i32 %result
}

!java-method-compilation = !{}

; WITH-LAG-LABEL: @current_iv_with_lag(
; WITH-LAG:       loop.exit:
; WITH-LAG-NEXT:    %last.lcssa = phi i32 [ %last.outer, %header.outer ]
; WITH-LAG:       header.outer:
; WITH-LAG:         %last.outer = phi i32 [ -1, %header.outer.ph ], [ %last.outer.next, %header.outer.latch ]
; WITH-LAG:       header.outer.latch:
; WITH-LAG:         %last.outer.next = phi i32 [ %iv, %latch ]
; WITH-LAG:         call hotspotcc void @jeandle.safepoint_poll()

;--- without-lag.ll

declare hotspotcc void @jeandle.safepoint_poll()

define i32 @current_iv_without_lag(i32 noundef %n) "java-method" {
entry:
  %entry.guard = icmp slt i32 0, %n
  br i1 %entry.guard, label %loop.preheader, label %return

loop.preheader:
  br label %header

header:
  %iv = phi i32 [ 0, %loop.preheader ], [ %iv.next, %latch ]
  br label %latch

latch:
  %iv.next = add nuw nsw i32 %iv, 1
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i32 %iv.next) ]
  %cond = icmp slt i32 %iv.next, %n
  br i1 %cond, label %header, label %loop.exit

loop.exit:
  %iv.lcssa = phi i32 [ %iv, %latch ]
  br label %return

return:
  %result = phi i32 [ -1, %entry ], [ %iv.lcssa, %loop.exit ]
  ret i32 %result
}

!java-method-compilation = !{}

; WITHOUT-LAG-LABEL: @current_iv_without_lag(
; WITHOUT-LAG:       latch:
; WITHOUT-LAG:         call hotspotcc void @jeandle.safepoint_poll()
; WITHOUT-LAG:       loop.exit:
; WITHOUT-LAG-NEXT:    %iv.lcssa = phi i32 [ %iv, %latch ]
; WITHOUT-LAG-NOT:   header.outer:
