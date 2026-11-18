; RUN: opt -verify-each -jeandle-loop-strip-mining-iter=4 -jeandle-safepoint-max-work=0 -passes='safepoint-poll-elimination<after-strip-mining>' -S < %s | FileCheck %s --check-prefix=EXHAUSTED
; RUN: opt -verify-each -jeandle-loop-strip-mining-iter=4 -passes="safepoint-poll-elimination<after-strip-mining>,verify<jeandle-safepoint-coverage>" -S < %s | FileCheck %s

declare hotspotcc void @jeandle.safepoint_poll()
declare hotspotcc void @java_method()

define void @short_split(ptr %flag, ptr %sink) "java-method" {
entry: br label %loop
loop:
  %i = phi i32 [0, %entry], [%next, %latch]
  %c = load volatile i1, ptr %flag
  br i1 %c, label %left, label %right
left:
  store volatile i32 %i, ptr %sink
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 %i)]
  br label %latch
right:
  store volatile i32 %i, ptr %sink
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 %i)]
  br label %latch
latch:
  %next = add nuw nsw i32 %i, 1
  %again = icmp ult i32 %next, 4
  br i1 %again, label %loop, label %exit
exit: ret void
}





define void @partial_call_coverage(ptr %flag, ptr %run) "java-method" {
entry: br label %loop
loop:
  %c = load volatile i1, ptr %flag
  br i1 %c, label %left, label %right
left:
  call hotspotcc void @java_method() ["deopt"()]
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"()]
  br label %latch
right:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"()]
  br label %latch
latch:
  %again = load volatile i1, ptr %run
  br i1 %again, label %loop, label %exit
exit: ret void
}


; CHECK-LABEL: define void @short_split(
; CHECK-NOT: call hotspotcc void @jeandle.safepoint_poll
; CHECK: ret void
; CHECK-LABEL: define void @partial_call_coverage(
; CHECK: left:
; CHECK: call hotspotcc void @java_method()
; CHECK-NOT: call hotspotcc void @jeandle.safepoint_poll
; CHECK: right:
; CHECK-NEXT: call hotspotcc void @jeandle.safepoint_poll
; CHECK: ret void

!java-method-compilation = !{}

define void @mandatory_child(i1 %enter, i1 %inner, i1 %outer) "java-method" {
entry: br label %header
header:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 10)]
  br label %child
child:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 20)]
  br i1 %inner, label %child, label %latch
latch:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 30)]
  br i1 %outer, label %header, label %exit
exit: ret void
}
; CHECK-LABEL: define void @mandatory_child(
; CHECK: header:
; CHECK-NOT: call hotspotcc void @jeandle.safepoint_poll
; CHECK: child:
; CHECK-NEXT: call hotspotcc void @jeandle.safepoint_poll(){{.*}}"deopt"(i32 20)
; CHECK: latch:
; CHECK-NOT: call hotspotcc void @jeandle.safepoint_poll
; CHECK: ret void

define void @zero_trip_child(i1 %enter, i1 %inner, i1 %outer) "java-method" {
entry: br label %header
header:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 10)]
  br i1 %enter, label %child, label %latch
child:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 20)]
  br i1 %inner, label %child, label %latch
latch:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 30)]
  br i1 %outer, label %header, label %exit
exit: ret void
}
; CHECK-LABEL: define void @zero_trip_child(
; CHECK: header:
; CHECK-NOT: call hotspotcc void @jeandle.safepoint_poll
; CHECK: child:
; CHECK-NEXT: call hotspotcc void @jeandle.safepoint_poll(){{.*}}"deopt"(i32 20)
; CHECK: latch:
; CHECK-NEXT: call hotspotcc void @jeandle.safepoint_poll(){{.*}}"deopt"(i32 30)
; CHECK: ret void

; EXHAUSTED-LABEL: define void @short_split(
; EXHAUSTED-COUNT-2: call hotspotcc void @jeandle.safepoint_poll()
; EXHAUSTED: ret void
; EXHAUSTED-LABEL: define void @partial_call_coverage(
; EXHAUSTED-COUNT-2: call hotspotcc void @jeandle.safepoint_poll()
; EXHAUSTED: ret void
