; RUN: opt -verify-each -passes=safepoint-poll-elimination -S %s | FileCheck %s
; RUN: opt -verify-each -passes='safepoint-poll-elimination<after-strip-mining>,safepoint-poll-elimination<after-strip-mining>' -S %s | FileCheck %s

; Keep branch-local polls by default: removing both arms' last side effect
; can expose costly speculation after loop unrolling. Full branch cuts are
; separately enabled and tested in cfg-poll-coalescing.ll.
declare hotspotcc void @jeandle.safepoint_poll()
declare void @java_call()
declare i32 @personality(...)

define i32 @diamond(i1 %choose, i32 %value) "java-method" {
entry:
  br i1 %choose, label %left, label %right
left:
  %a = mul i32 %value, 17
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 %a)]
  br label %exit
right:
  %b = mul i32 %value, 31
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 %b)]
  br label %exit
exit:
  %result = phi i32 [%a, %left], [%b, %right]
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 %result)]
  ret i32 %result
}
; CHECK-LABEL: define i32 @diamond(
; CHECK: "deopt"(i32 %a)
; CHECK: "deopt"(i32 %b)
; CHECK: "deopt"(i32 %result)

define void @chain() "java-method" {
entry:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 1)]
  br label %middle
middle:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 2)]
  br label %exit
exit:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 3)]
  ret void
}
; CHECK-LABEL: define void @chain(
; CHECK-NOT: @jeandle.safepoint_poll
; CHECK: exit:
; CHECK-NEXT: call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i32 3) ]
; CHECK-NEXT: ret void

define void @invoke_normal() "java-method" personality ptr @personality {
entry:
  invoke void @java_call() ["deopt"()] to label %normal unwind label %unwind
normal:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 1)]
  ret void
unwind:
  %exception = landingpad {ptr, i32} cleanup
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 2)]
  resume {ptr, i32} %exception
}
; CHECK-LABEL: define void @invoke_normal(
; CHECK: normal:
; CHECK-NEXT: ret void
; CHECK: unwind:
; CHECK: "deopt"(i32 2)

!java-method-compilation = !{}

; A shared normal successor is a merge, even when both calls can safepoint.
define void @shared_normal(i1 %choose) "java-method" personality ptr @personality {
entry:
  br i1 %choose, label %left, label %right
left:
  invoke void @java_call() ["deopt"()] to label %normal unwind label %unwind
right:
  invoke void @java_call() ["deopt"()] to label %normal unwind label %unwind
normal:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 3)]
  ret void
unwind:
  %exception = landingpad {ptr, i32} cleanup
  resume {ptr, i32} %exception
}
; CHECK-LABEL: define void @shared_normal(
; CHECK: normal:
; CHECK-NEXT: call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i32 3) ]

; A deoptimization side exit cannot be folded into speculative arithmetic.
; Keep coverage along the unique normal path after a Java call.
declare void @llvm.experimental.deoptimize.isVoid(...)
define void @call_guard(i1 %pass) "java-method" {
entry:
  call void @java_call() ["deopt"()]
  br i1 %pass, label %normal, label %deopt
normal:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 4)]
  ret void
deopt:
  call void (...) @llvm.experimental.deoptimize.isVoid() ["deopt"(i32 5)]
  ret void
}
; CHECK-LABEL: define void @call_guard(
; CHECK: normal:
; CHECK-NEXT: ret void
; CHECK: deopt:
; CHECK-NEXT: call void (...) @llvm.experimental.deoptimize.isVoid()
