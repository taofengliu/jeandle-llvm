; RUN: opt -passes='loop-simplify,lcssa,safepoint-poll-elimination<after-strip-mining>,verify<jeandle-safepoint-coverage>' -jeandle-loop-strip-mining-iter=4 -verify-each -S %s | FileCheck %s

; The outer poll's marker cannot exempt an unrelated, unbounded child. The
; poll in that child must survive elimination even though its sibling is short.
declare hotspotcc void @jeandle.safepoint_poll()

define void @marked_parent(ptr %flag, ptr %out, i1 %which) "java-method" {
entry:
  br label %outer
outer:
  br i1 %which, label %short.ph, label %unknown.ph
short.ph:
  br label %short
short:
  %i = phi i32 [ 0, %short.ph ], [ %next, %short ]
  %next = add nuw nsw i32 %i, 1
  store i32 %i, ptr %out
  %again = icmp ult i32 %next, 3
  br i1 %again, label %short, label %latch
unknown.ph:
  br label %unknown
unknown:
  %more = load volatile i1, ptr %flag
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"() ]
  br i1 %more, label %unknown, label %latch
latch:
  call hotspotcc void @jeandle.safepoint_poll() #0 [ "deopt"() ]
  br label %outer
}

; CHECK-LABEL: @marked_parent(
; CHECK: unknown:
; CHECK: call hotspotcc void @jeandle.safepoint_poll() [ "deopt"() ]
; CHECK: latch:
; CHECK: call hotspotcc void @jeandle.safepoint_poll() #{{[0-9]+}}

attributes #0 = { "jeandle.strip-mined-poll" }
!java-method-compilation = !{}
