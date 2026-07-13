; RUN: opt -passes='safepoint-elimination<early>,safepoint-elimination<strip-mining>' -jeandle-enable-strip-mining -S < %s | FileCheck %s

; A lifted recurrence can carry a loop-invariant latch value. Exit LCSSA fixup
; must not key remaps on that invariant, or an unrelated equal constant leaking
; out through the exit phi can be rewritten to the recurrence's outer phi.

declare hotspotcc void @jeandle.safepoint_poll()

define i64 @invariant_exit_phi_collision(i64 %n) {
entry:
  br label %header

header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %latch ]
  %x = phi i64 [ 7, %entry ], [ 42, %latch ]
  %cond = icmp slt i64 %iv, %n
  br i1 %cond, label %body, label %exit

body:
  %iv.next = add i64 %iv, 1
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next, i64 42) ]
  br label %latch

latch:
  br label %header

exit:
  %r = phi i64 [ 42, %header ]
  ret i64 %r
}

!java-method-compilation = !{}

; CHECK-LABEL: @invariant_exit_phi_collision(
; CHECK:       exit:
; CHECK:         %r = phi i64 [ 42, %header.outer ]
; CHECK:       header.outer:
; CHECK:         %x.outer = phi i64 [ 7, %header.outer.ph ], [ %x.outer.next, %header.outer.latch ]
