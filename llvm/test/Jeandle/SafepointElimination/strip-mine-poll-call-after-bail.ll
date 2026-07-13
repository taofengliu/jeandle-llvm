; RUN: opt -passes='safepoint-elimination<early>,safepoint-elimination<strip-mining>' \
; RUN:   -jeandle-enable-strip-mining -S < %s | FileCheck %s

declare hotspotcc void @jeandle.safepoint_poll()
declare void @readonly_call(ptr) memory(read) nounwind

define void @call_after_poll_bails(i64 %n, ptr %p) {
entry:
  br label %header
header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %latch ]
  %cond = icmp slt i64 %iv, %n
  br i1 %cond, label %body, label %exit
body:
  %iv.next = add i64 %iv, 1
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
  call void @readonly_call(ptr %p)
  br label %latch
latch:
  br label %header
exit:
  ret void
}

; CHECK-LABEL: @call_after_poll_bails(
; CHECK-NOT:   .outer
; CHECK:       call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
; CHECK-NEXT:  call void @readonly_call(ptr %p)

!java-method-compilation = !{}
