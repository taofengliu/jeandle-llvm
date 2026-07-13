; RUN: opt -passes='safepoint-elimination<early>,safepoint-elimination<strip-mining>,verify<jeandle-safepoint-coverage>' \
; RUN:   -jeandle-enable-strip-mining -jeandle-verify-safepoint-coverage=fatal \
; RUN:   -S < %s 2>&1 | FileCheck %s

; After strip mining, the inner loop runs poll-free with a bound SCEV can't
; recover (clamped select). The fatal coverage verifier must still pass: the
; inner latch carries !strip-mined (bounded by construction) and the
; outer loop carries the relocated poll. A coverage violation would abort the
; compile, so reaching FileCheck at all proves the verifier accepted the nest.

declare hotspotcc void @jeandle.safepoint_poll()

define void @loop(i64 %n) {
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

!java-method-compilation = !{}

; CHECK-NOT: SafepointCoverageVerifier
; CHECK:     br label %header, !strip-mined
; CHECK:     call hotspotcc void @jeandle.safepoint_poll(){{.*}}!poll-coverage
