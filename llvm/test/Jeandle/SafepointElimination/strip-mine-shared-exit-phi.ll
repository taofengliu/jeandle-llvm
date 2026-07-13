; RUN: opt -passes='safepoint-elimination<early>,safepoint-elimination<strip-mining>,verify' -jeandle-enable-strip-mining -S < %s | FileCheck %s

; One header recurrence (%s) leaks out through two separate exit LCSSA phis.
; The exit fixup resolves each exit phi by its incoming value (header phi ->
; outer phi), so both are repointed at the outer recurrence. A map keyed the
; other way (header phi -> single exit phi) would clobber one of them, leaving
; it referencing the header phi from an OuterHeader edge the header phi does not
; dominate -- invalid IR. The RUN line pipes through verify to catch exactly
; that.

declare hotspotcc void @jeandle.safepoint_poll()

define i64 @dup(i64 %n) {
entry:
  br label %header

header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %latch ]
  %s  = phi i64 [ 0, %entry ], [ %s.next, %latch ]
  %cond = icmp slt i64 %iv, %n
  br i1 %cond, label %body, label %exit

body:
  %s.next = add i64 %s, %iv
  %iv.next = add i64 %iv, 1
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %s.next) ]
  br label %latch

latch:
  br label %header

exit:
  %r1 = phi i64 [ %s, %header ]
  %r2 = phi i64 [ %s, %header ]
  %sum = add i64 %r1, %r2
  ret i64 %sum
}

!java-method-compilation = !{}

; Both exit phis fix up to the outer recurrence, and the loop is strip-mined.
; CHECK-LABEL: @dup(
; CHECK:       exit:
; CHECK:         %r1 = phi i64 [ %s.outer, %header.outer ]
; CHECK:         %r2 = phi i64 [ %s.outer, %header.outer ]
; CHECK:       header.outer.latch:
; CHECK:         call hotspotcc void @jeandle.safepoint_poll(){{.*}}!poll-coverage
