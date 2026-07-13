; RUN: opt -passes='safepoint-elimination<early>,safepoint-elimination<strip-mining>,verify' \
; RUN:   -jeandle-enable-strip-mining -S < %s | FileCheck %s

declare hotspotcc void @jeandle.safepoint_poll()

define void @store_before_poll(i64 %n, ptr %p) {
entry:
  br label %header
header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %latch ]
  %cond = icmp slt i64 %iv, %n
  br i1 %cond, label %body, label %exit
body:
  store volatile i64 %iv, ptr %p
  %iv.next = add i64 %iv, 1
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
  br label %latch
latch:
  br label %header
exit:
  ret void
}

; CHECK-LABEL: @store_before_poll(
; CHECK:       header.outer:
; CHECK:       call hotspotcc void @jeandle.safepoint_poll(){{.*}}!poll-coverage

define void @store_after_poll_bails(i64 %n, ptr %p) {
entry:
  br label %header
header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %latch ]
  %cond = icmp slt i64 %iv, %n
  br i1 %cond, label %body, label %exit
body:
  %iv.next = add i64 %iv, 1
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
  store volatile i64 %iv, ptr %p
  br label %latch
latch:
  br label %header
exit:
  ret void
}

; CHECK-LABEL: @store_after_poll_bails(
; CHECK-NOT:   .outer
; CHECK:       call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
; CHECK:       store volatile i64 %iv, ptr %p

define void @store_on_one_backedge_path_bails(i64 %n, ptr %p, i1 %write) {
entry:
  br label %header
header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %latch ]
  %cond = icmp slt i64 %iv, %n
  br i1 %cond, label %body, label %exit
body:
  %iv.next = add i64 %iv, 1
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
  br i1 %write, label %write.path, label %clean.path
write.path:
  store volatile i64 %iv, ptr %p
  br label %latch
clean.path:
  br label %latch
latch:
  br label %header
exit:
  ret void
}

; CHECK-LABEL: @store_on_one_backedge_path_bails(
; CHECK-NOT:   .outer
; CHECK:       call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
; CHECK:       store volatile i64 %iv, ptr %p

define void @side_exit_store_is_irrelevant(i64 %n, ptr %p, i1 %leave) {
entry:
  br label %header
header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %latch ]
  %cond = icmp slt i64 %iv, %n
  br i1 %cond, label %body, label %exit
body:
  %iv.next = add i64 %iv, 1
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
  br i1 %leave, label %side.exit, label %latch
latch:
  br label %header
side.exit:
  store volatile i64 7, ptr %p
  ret void
exit:
  ret void
}

; CHECK-LABEL: @side_exit_store_is_irrelevant(
; CHECK:       side.exit:
; CHECK:       store volatile i64 7, ptr %p
; CHECK:       header.outer:
; CHECK:       call hotspotcc void @jeandle.safepoint_poll(){{.*}}!poll-coverage

define void @ordinary_load_after_poll_is_allowed(i64 %n, ptr %p) {
entry:
  br label %header
header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %latch ]
  %cond = icmp slt i64 %iv, %n
  br i1 %cond, label %body, label %exit
body:
  %iv.next = add i64 %iv, 1
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
  %loaded = load i64, ptr %p
  br label %latch
latch:
  br label %header
exit:
  ret void
}

; CHECK-LABEL: @ordinary_load_after_poll_is_allowed(
; CHECK:       %loaded = load i64, ptr %p
; CHECK:       header.outer:
; CHECK:       call hotspotcc void @jeandle.safepoint_poll(){{.*}}!poll-coverage

define void @unordered_atomic_load_after_poll_bails(i64 %n, ptr %p) {
entry:
  br label %header
header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %latch ]
  %cond = icmp slt i64 %iv, %n
  br i1 %cond, label %body, label %exit
body:
  %iv.next = add i64 %iv, 1
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
  %loaded = load atomic i64, ptr %p unordered, align 8
  br label %latch
latch:
  br label %header
exit:
  ret void
}

; CHECK-LABEL: @unordered_atomic_load_after_poll_bails(
; CHECK-NOT:   .outer
; CHECK:       call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
; CHECK-NEXT:  %loaded = load atomic i64, ptr %p unordered, align 8

!java-method-compilation = !{}
