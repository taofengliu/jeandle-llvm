; RUN: opt -passes='safepoint-elimination<strip-mining>' \
; RUN:   -jeandle-enable-strip-mining -S < %s | FileCheck %s
; RUN: opt -passes='safepoint-elimination<strip-mining>' \
; RUN:   -S < %s | FileCheck %s --check-prefix=DISABLED \
; RUN:   --implicit-check-not=.outer --implicit-check-not='!poll-coverage'

; StripMining does not perform keep-one or short-loop deletion while preparing
; MemorySSA-based relocation plans.

declare hotspotcc void @jeandle.safepoint_poll()

define void @non_counted_keeps_both_polls(i1 %again) {
entry:
  br label %header
header:
  call hotspotcc void @jeandle.safepoint_poll()
  br label %body
body:
  call hotspotcc void @jeandle.safepoint_poll()
  br i1 %again, label %header, label %exit
exit:
  ret void
}

; CHECK-LABEL: @non_counted_keeps_both_polls(
; CHECK-COUNT-2: call hotspotcc void @jeandle.safepoint_poll()
; CHECK-NOT:   !poll-coverage

define void @short_counted_loop_keeps_poll() {
entry:
  br label %header
header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %latch ]
  %cond = icmp slt i64 %iv, 4
  br i1 %cond, label %body, label %exit
body:
  %iv.next = add i64 %iv, 1
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
  br label %latch
latch:
  br label %header
exit:
  ret void
}

; CHECK-LABEL: @short_counted_loop_keeps_poll(
; CHECK-NOT:   .outer
; CHECK:       call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]

define void @strip_mining_toggle(i64 %n) {
entry:
  br label %header
header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %latch ]
  %cond = icmp slt i64 %iv, %n
  br i1 %cond, label %body, label %exit
body:
  %iv.next = add i64 %iv, 1
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
  br label %latch
latch:
  br label %header
exit:
  ret void
}

; DISABLED-LABEL: @strip_mining_toggle(
; DISABLED:       body:
; DISABLED:         call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]

; CHECK-LABEL: @strip_mining_toggle(
; CHECK:       body:
; CHECK-NOT:     call hotspotcc void @jeandle.safepoint_poll
; CHECK:       header.outer.latch:
; CHECK:         call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %outer.iv.next) ]{{.*}}!poll-coverage

!java-method-compilation = !{}
