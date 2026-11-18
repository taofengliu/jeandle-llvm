; RUN: opt -passes='verify<jeandle-safepoint-coverage>' -jeandle-loop-strip-mining-iter=4 -jeandle-verify-safepoint-coverage=warn -disable-output %s 2>&1 | FileCheck %s

; A marked nest with an ordinary bitwise limit needs a real bound proof. It
; must produce a coverage diagnostic without querying add/sub flags on an and.
declare hotspotcc void @jeandle.safepoint_poll()
define void @non_clamp_bound(i64 %raw) "java-method" {
entry:
  br label %outer
outer:
  %base = phi i64 [ 0, %entry ], [ %last, %latch ]
  %limit = and i64 %raw, 4095
  br label %inner
inner:
  %iv = phi i64 [ %base, %outer ], [ %next, %inner ]
  %next = add nsw i64 %iv, 1
  %keep = icmp slt i64 %next, %limit
  br i1 %keep, label %inner, label %latch
latch:
  %last = phi i64 [ %next, %inner ]
  call hotspotcc void @jeandle.safepoint_poll() #0 [ "deopt"() ]
  br label %outer
}
; CHECK: SafepointCoverageVerifier: loop with header 'inner' in function 'non_clamp_bound'
attributes #0 = { "jeandle.strip-mined-poll" }
!java-method-compilation = !{}
