; RUN: opt -passes='verify<jeandle-safepoint-coverage>' -jeandle-loop-strip-mining-iter=4 -jeandle-verify-safepoint-coverage=warn -disable-output %s 2>&1 | FileCheck %s

; An unsigned add of -1 with nuw is an addition of UINT_MAX. For a defined
; zero base it produces UINT_MAX, not a value below the base. It cannot be
; treated as a countdown prologue when validating the following inner bound.
declare hotspotcc void @jeandle.safepoint_poll()

define void @unsigned_progress() "java-method" {
entry:
  br label %outer
outer:
  %base = phi i64 [ 0, %entry ], [ 0, %latch ]
  %start = add nuw i64 %base, -1
  %limit = sub nuw i64 %base, 0
  br label %inner
inner:
  %iv = phi i64 [ %start, %outer ], [ %next, %inner ]
  %next = add i64 %iv, -1
  %keep = icmp ugt i64 %next, %limit
  br i1 %keep, label %inner, label %latch
latch:
  call hotspotcc void @jeandle.safepoint_poll() #0 [ "deopt"() ]
  br label %outer
}

; CHECK: SafepointCoverageVerifier: loop with header 'inner' in function 'unsigned_progress'
attributes #0 = { "jeandle.strip-mined-poll" }
!java-method-compilation = !{}
