; RUN: opt -passes=safepoint-elimination -S < %s | FileCheck %s

; Two polls separated by a load — a real (non-transparent) instruction. R3 must
; not collapse them; the load's program point needs the deopt anchor between
; the polls. Kept on a straight-line path (no loop) so that keep-one dedup,
; which would legitimately drop one of two latch-dominating polls, doesn't mask
; what R3 alone does.

declare hotspotcc void @jeandle.safepoint_poll()

define void @not_adjacent(ptr %a) gc "safepoint-in-loop-example" {
entry:
  call hotspotcc void @jeandle.safepoint_poll()
  %v = load i32, ptr %a
  call hotspotcc void @jeandle.safepoint_poll()
  ret void
}

!java-method-compilation = !{}

; CHECK-LABEL: @not_adjacent(
; CHECK:         call hotspotcc void @jeandle.safepoint_poll()
; CHECK-NEXT:    load i32, ptr %a
; CHECK-NEXT:    call hotspotcc void @jeandle.safepoint_poll()
; CHECK-NEXT:    ret void
