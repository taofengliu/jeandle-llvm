; RUN: opt -passes='safepoint-elimination,verify<jeandle-safepoint-coverage>' \
; RUN:   -jeandle-verify-safepoint-coverage=fatal -S < %s | FileCheck %s
; RUN: opt -passes='safepoint-elimination,verify<jeandle-safepoint-coverage>' \
; RUN:   -jeandle-enable-strip-mining -jeandle-verify-safepoint-coverage=fatal \
; RUN:   -S < %s | FileCheck %s

; A zero-trip inner loop is short enough to delete its own poll, but that poll
; can still be the enclosing infinite loop's only reachable safepoint. Keep it.

declare hotspotcc void @jeandle.safepoint_poll()

define void @keeps_outer_required_inner_poll() {
entry:
  br label %outer.header

outer.header:
  br label %inner.header

inner.header:
  call hotspotcc void @jeandle.safepoint_poll()
  br i1 false, label %inner.latch, label %outer.latch

inner.latch:
  br label %inner.header

outer.latch:
  br label %outer.header
}

; CHECK-LABEL: @keeps_outer_required_inner_poll(
; CHECK:       inner.header:
; CHECK-NEXT:    call hotspotcc void @jeandle.safepoint_poll()

; Even when the short loop is counted, enabling strip mining must not relocate
; away the ancestor-required poll after short-loop deletion declines to erase it.
define void @keeps_outer_required_counted_poll_with_strip_mining() {
entry:
  br label %outer.header

outer.header:
  br label %inner.header

inner.header:
  %iv = phi i64 [ 0, %outer.header ], [ %iv.next, %inner.latch ]
  call hotspotcc void @jeandle.safepoint_poll()
  %cond = icmp slt i64 %iv, 2
  br i1 %cond, label %inner.latch, label %outer.latch

inner.latch:
  %iv.next = add nuw nsw i64 %iv, 1
  br label %inner.header

outer.latch:
  br label %outer.header
}

; CHECK-LABEL: @keeps_outer_required_counted_poll_with_strip_mining(
; CHECK:       inner.header:
; CHECK:         call hotspotcc void @jeandle.safepoint_poll()

; Strip mining must also leave a non-short counted inner loop alone when moving
; its backedge poll would uncover the enclosing loop.
define void @keeps_outer_required_nonshort_counted_poll(i64 %n) {
entry:
  br label %outer.header

outer.header:
  br label %inner.header

inner.header:
  %iv = phi i64 [ 0, %outer.header ], [ %iv.next, %inner.latch ]
  call hotspotcc void @jeandle.safepoint_poll()
  %cond = icmp slt i64 %iv, %n
  br i1 %cond, label %inner.latch, label %outer.latch

inner.latch:
  %iv.next = add nuw nsw i64 %iv, 1
  br label %inner.header

outer.latch:
  br label %outer.header
}

; CHECK-LABEL: @keeps_outer_required_nonshort_counted_poll(
; CHECK-NOT:   .outer
; CHECK:       inner.header:
; CHECK:         call hotspotcc void @jeandle.safepoint_poll()

; keep-one must not prefer a later inner-loop poll if an earlier poll is the one
; that covers the enclosing loop's latch.
define void @keeps_outer_required_poll_when_keep_one_runs() {
entry:
  br label %outer.header

outer.header:
  br label %inner.header

inner.header:
  call hotspotcc void @jeandle.safepoint_poll()
  br i1 false, label %inner.latch, label %outer.latch

inner.latch:
  call hotspotcc void @jeandle.safepoint_poll()
  br label %inner.header

outer.latch:
  br label %outer.header
}

; CHECK-LABEL: @keeps_outer_required_poll_when_keep_one_runs(
; CHECK:       inner.header:
; CHECK-NEXT:    call hotspotcc void @jeandle.safepoint_poll()
; CHECK:       inner.latch:
; CHECK-NEXT:    br label %inner.header

; With strip mining enabled, keep-one still has to prefer the ancestor-required
; header poll over a later latch poll before the strip-mining decision runs.
define void @keeps_ancestor_required_poll_before_strip_mining_keep_one(i64 %n) {
entry:
  br label %outer.header

outer.header:
  br label %inner.header

inner.header:
  %iv = phi i64 [ 0, %outer.header ], [ %iv.next, %inner.latch ]
  call hotspotcc void @jeandle.safepoint_poll()
  %cond = icmp slt i64 %iv, %n
  br i1 %cond, label %inner.latch, label %outer.latch

inner.latch:
  call hotspotcc void @jeandle.safepoint_poll()
  %iv.next = add nuw nsw i64 %iv, 1
  br label %inner.header

outer.latch:
  br label %outer.header
}

; CHECK-LABEL: @keeps_ancestor_required_poll_before_strip_mining_keep_one(
; CHECK-NOT:   .outer
; CHECK:       inner.header:
; CHECK:         call hotspotcc void @jeandle.safepoint_poll()
; CHECK:       inner.latch:
; CHECK-NEXT:    %iv.next = add nuw nsw i64 %iv, 1
; CHECK-NEXT:    br label %inner.header

; A bounded immediate parent is not enough reason to delete the poll if an
; unbounded grandparent still depends on it.
define void @keeps_transitive_outer_required_inner_poll() {
entry:
  br label %outer.header

outer.header:
  br label %middle.header

middle.header:
  %mid = phi i64 [ 0, %outer.header ], [ %mid.next, %middle.latch ]
  br label %inner.header

inner.header:
  call hotspotcc void @jeandle.safepoint_poll()
  br i1 false, label %inner.latch, label %middle.latch

inner.latch:
  br label %inner.header

middle.latch:
  %mid.next = add nuw nsw i64 %mid, 1
  %middle.cond = icmp slt i64 %mid.next, 2
  br i1 %middle.cond, label %middle.header, label %outer.latch

outer.latch:
  br label %outer.header
}

; CHECK-LABEL: @keeps_transitive_outer_required_inner_poll(
; CHECK:       inner.header:
; CHECK-NEXT:    call hotspotcc void @jeandle.safepoint_poll()

; If the ancestor loop has its own dominating poll, the short inner loop's poll
; is not ancestor-required and can still be deleted.
define void @deletes_inner_poll_when_outer_has_own_poll() {
entry:
  br label %outer.header

outer.header:
  call hotspotcc void @jeandle.safepoint_poll()
  br label %inner.header

inner.header:
  call hotspotcc void @jeandle.safepoint_poll()
  br i1 false, label %inner.latch, label %outer.latch

inner.latch:
  br label %inner.header

outer.latch:
  br label %outer.header
}

; CHECK-LABEL: @deletes_inner_poll_when_outer_has_own_poll(
; CHECK:       outer.header:
; CHECK-NEXT:    call hotspotcc void @jeandle.safepoint_poll()
; CHECK:       inner.header:
; CHECK-NOT:     call hotspotcc void @jeandle.safepoint_poll()
; CHECK:         br i1 false, label %inner.latch, label %outer.latch

!java-method-compilation = !{}
