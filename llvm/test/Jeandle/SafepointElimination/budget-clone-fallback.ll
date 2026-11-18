; RUN: opt -verify-each -jeandle-loop-strip-mining-iter=4 -jeandle-safepoint-max-loop-clone-instructions=0 -passes='lcssa,safepoint-budget-strip-mining' -S %s | FileCheck %s --check-prefix=OFF
; RUN: opt -verify-each -jeandle-loop-strip-mining-iter=4 -jeandle-safepoint-max-function-clone-instructions=0 -passes='lcssa,safepoint-budget-strip-mining' -S %s | FileCheck %s --check-prefix=OFF
; RUN: opt -verify-each -jeandle-loop-strip-mining-iter=4 -passes='lcssa,safepoint-budget-strip-mining,safepoint-budget-strip-mining,safepoint-poll-elimination<after-strip-mining>,verify<jeandle-safepoint-coverage>' -S %s | FileCheck %s
; RUN: opt -verify-each -jeandle-loop-strip-mining-iter=4 -passes='lcssa,safepoint-budget-strip-mining,safepoint-poll-elimination<after-strip-mining>,verify<jeandle-safepoint-coverage>' -S %s | FileCheck %s
; RUN: opt -verify-each -jeandle-loop-strip-mining-iter=2 -passes='lcssa,safepoint-budget-strip-mining,safepoint-poll-elimination<after-strip-mining>,verify<jeandle-safepoint-coverage>' -S %s | FileCheck %s --check-prefix=TWO
; RUN: opt -verify-each -jeandle-loop-strip-mining-iter=18446744073709551615 -passes='lcssa,safepoint-budget-strip-mining,safepoint-poll-elimination<after-strip-mining>,verify<jeandle-safepoint-coverage>' -S %s | FileCheck %s --check-prefix=MAX

; Different poll states and a store after each poll forbid relocation. The
; preheader side effect and source-header load must keep their entry counts.
declare hotspotcc void @jeandle.safepoint_poll()
declare void @preheader_work()

define i32 @split_states(ptr %out, ptr %flag, i32 %start, i32 %limit,
                         ptr %a, ptr %b) "java-method" {
entry:
  call void @preheader_work()
  br label %header
header:
  %iv = phi i32 [%start, %entry], [%next, %latch]
  %self = phi i32 [42, %entry], [%self, %latch]
  %left.oop = phi ptr [%a, %entry], [%right.oop, %latch]
  %right.oop = phi ptr [%b, %entry], [%left.oop, %latch]
  %choose = load volatile i1, ptr %flag
  %done = icmp eq i32 %iv, %limit
  br i1 %done, label %exit, label %body
body:
  br i1 %choose, label %left, label %right
left:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 10, i32 %iv, i32 %self, ptr %left.oop)]
  store volatile i32 %iv, ptr %out
  br label %latch
right:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 20, i32 %iv, i32 %self, ptr %right.oop)]
  store volatile i32 %iv, ptr %out
  br label %latch
latch:
  %stride = select i1 %choose, i32 1, i32 3
  %next = add i32 %iv, %stride
  br label %header
exit:
  %result = phi i32 [%iv, %header]
  ret i32 %result
}

; CHECK-LABEL: define i32 @split_states(
; CHECK: call void @preheader_work()
; CHECK-NOT: call void @preheader_work()
; CHECK: batch.outer:
; CHECK: phi i32 [ %start, %entry ], [ %next, %latch ]
; CHECK: phi ptr [ %a, %entry ], [ %right.oop, %latch ]
; CHECK: header.fast:
; CHECK: %batch.tick = phi i64
; CHECK: %self.fast = phi i32
; CHECK: %left.oop.fast = phi ptr
; CHECK: load volatile i1, ptr %flag
; CHECK-NOT: call hotspotcc void @jeandle.safepoint_poll
; CHECK: right.fast:
; CHECK-NOT: call hotspotcc void @jeandle.safepoint_poll
; CHECK: store volatile i32 %iv.fast
; CHECK: left.fast:
; CHECK-NOT: call hotspotcc void @jeandle.safepoint_poll
; CHECK: store volatile i32 %iv.fast
; CHECK: header:
; CHECK: %iv = phi i32 [ %next.fast, %batch.check ]
; CHECK: %self = phi i32 [ %self.fast, %batch.check ]
; CHECK: %left.oop = phi ptr [ %right.oop.fast, %batch.check ]
; CHECK: %right.oop = phi ptr [ %left.oop.fast, %batch.check ]
; CHECK: load volatile i1, ptr %flag
; CHECK: left:
; CHECK: call hotspotcc void @jeandle.safepoint_poll() {{.*}}"deopt"(i32 10, i32 %iv, i32 %self, ptr %left.oop)
; CHECK-NEXT: store volatile i32 %iv
; CHECK: right:
; CHECK: call hotspotcc void @jeandle.safepoint_poll() {{.*}}"deopt"(i32 20, i32 %iv, i32 %self, ptr %right.oop)
; CHECK-NEXT: store volatile i32 %iv
; CHECK: latch:
; CHECK: br label %batch.outer
; CHECK: exit:
; CHECK: %result = phi i32 [ %iv, %header ], [ %iv.fast, %header.fast ]
; CHECK: batch.check:
; CHECK: %batch.tick.next = add nuw nsw i64 %batch.tick, 1
; CHECK: %batch.more = icmp ult i64 %batch.tick.next, 3
; CHECK: br i1 %batch.more, label %header.fast, label %header
; TWO-LABEL: define i32 @split_states(
; TWO: %batch.more = icmp ult i64 %batch.tick.next, 1
; MAX-LABEL: define i32 @split_states(
; MAX: %batch.tick.next = add nuw i64 %batch.tick, 1
; MAX: %batch.more = icmp ult i64 %batch.tick.next, -2

!java-method-compilation = !{}

; OFF-LABEL: define i32 @split_states(
; OFF-NOT: batch.outer
; OFF-NOT: .fast
; OFF: call hotspotcc void @jeandle.safepoint_poll() {{.*}}"deopt"(i32 10
; OFF: call hotspotcc void @jeandle.safepoint_poll() {{.*}}"deopt"(i32 20
; OFF: ret i32

declare hotspotcc i32 @java_method(i32)
define i32 @mixed_calls(ptr %flag, ptr %out, i32 %start, i32 %limit) "java-method" {
entry: br label %header
header:
  %iv = phi i32 [%start, %entry], [%next, %latch]
  %choose = load volatile i1, ptr %flag
  br i1 %choose, label %left, label %right
left:
  %called = call hotspotcc i32 @java_method(i32 %iv) ["deopt"(i32 %iv)]
  br label %latch
right:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 %iv)]
  br label %latch
latch:
  %step = phi i32 [%called, %left], [1, %right]
  %next = add i32 %iv, %step
  store volatile i32 %next, ptr %out
  %more = icmp ne i32 %next, %limit
  br i1 %more, label %header, label %exit
exit:
  %result = phi i32 [%next, %latch]
  ret i32 %result
}
; CHECK-LABEL: define i32 @mixed_calls(
; CHECK: batch.outer:
; CHECK: header.fast:
; CHECK: call hotspotcc i32 @java_method(i32 %iv.fast) {{.*}}"deopt"(i32 %iv.fast)
; CHECK: header:
; CHECK: call hotspotcc i32 @java_method(i32 %iv) {{.*}}"deopt"(i32 %iv)
; CHECK: call hotspotcc void @jeandle.safepoint_poll()
; CHECK: ret i32
