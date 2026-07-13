; RUN: opt -passes='safepoint-elimination<cleanup>' -jeandle-enable-strip-mining -S < %s \
; RUN:   | FileCheck %s --check-prefix=CLEANUP --implicit-check-not=.outer
; RUN: opt -passes='safepoint-elimination<early>' -jeandle-enable-strip-mining -S < %s \
; RUN:   | FileCheck %s --check-prefix=CLEANUP --implicit-check-not=.outer
; RUN: opt -passes='safepoint-elimination<early>,safepoint-elimination<strip-mining>' -jeandle-enable-strip-mining -S < %s \
; RUN:   | FileCheck %s --check-prefix=FIRST

; This models the IR shape the pipeline second pass is meant to clean up: an
; intervening loop transform duplicated an existing poll in the same loop. The
; cleanup mode should dedup that poll, but must not create a strip-mined loop
; even when strip mining is globally enabled.

declare hotspotcc void @jeandle.safepoint_poll()

define void @cleanup_dedups_post_transform_duplicate(i64 %n) {
entry:
  br label %h

h:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %lat ]
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv) ]
  %c = icmp slt i64 %iv, %n
  br i1 %c, label %b, label %x

b:
  %iv.next = add i64 %iv, 1
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
  br label %lat

lat:
  br label %h

x:
  ret void
}

!java-method-compilation = !{}

; CLEANUP-LABEL: @cleanup_dedups_post_transform_duplicate(
; CLEANUP:       h:
; CLEANUP-NOT:     call hotspotcc void @jeandle.safepoint_poll
; CLEANUP:         br i1 %c, label %b, label %x
; CLEANUP:       b:
; CLEANUP-NEXT:    %iv.next = add i64 %iv, 1
; CLEANUP-NEXT:    call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]{{.*}}!poll-coverage
; CLEANUP-NOT:     call hotspotcc void @jeandle.safepoint_poll
; CLEANUP:       x:
; CLEANUP-NEXT:    ret void

; FIRST-LABEL: @cleanup_dedups_post_transform_duplicate(
; FIRST:       h.outer.inner.entry:
; FIRST:       h.outer.latch:
; FIRST:         call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %outer.iv.next) ]{{.*}}!poll-coverage
