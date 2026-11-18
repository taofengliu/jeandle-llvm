; RUN: opt -passes='loop-simplify,lcssa,safepoint-poll-elimination<after-strip-mining>,verify<jeandle-safepoint-coverage>' -jeandle-loop-strip-mining-iter=4 -jeandle-verify-safepoint-coverage=fatal -verify-each -S %s | FileCheck %s

; A negative-step unsigned AddRec with nuw may advance from zero to
; UINT64_MAX. It is not a monotonic countdown prologue. The inner poll is
; required: on the second outer iteration the inner runs UINT64_MAX times.
; CHECK-LABEL: @unsigned_ar_progress(
; CHECK: inner:
; CHECK: call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %next) ]
; CHECK: latch:
; CHECK: call hotspotcc void @jeandle.safepoint_poll() #{{[0-9]+}}

declare hotspotcc void @jeandle.safepoint_poll()
define void @unsigned_ar_progress(i64 noundef %base) "java-method" {
entry:
  %zero = icmp eq i64 %base, 0
  br i1 %zero, label %outer, label %exit
outer:
  %p = phi i64 [ %base, %entry ], [ %pnext, %latch ]
  %skip = icmp eq i64 %p, 0
  br i1 %skip, label %latch, label %pre
pre:
  %limit = sub nuw i64 %base, 0
  br label %inner
inner:
  %iv = phi i64 [ %p, %pre ], [ %next, %inner ]
  %next = sub nuw i64 %iv, 1
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %next) ]
  %keep = icmp ugt i64 %next, %limit
  br i1 %keep, label %inner, label %latch
latch:
  call hotspotcc void @jeandle.safepoint_poll() #0 [ "deopt"() ]
  %pnext = add nuw i64 %p, -1
  br i1 %skip, label %outer, label %exit
exit:
  ret void
}
attributes #0 = { "jeandle.strip-mined-poll" }
!java-method-compilation = !{}
