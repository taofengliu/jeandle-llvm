; REQUIRES: native
; RUN: opt -verify-each -jeandle-loop-strip-mining-iter=4 -passes='loop-simplify,lcssa,safepoint-budget-strip-mining,safepoint-poll-elimination<after-strip-mining>,verify<jeandle-safepoint-coverage>' -S %s -o %t
; RUN: FileCheck %s < %t
; RUN: lli %t
; RUN: opt -verify-each -passes='java-operation-lower<phase=1>,default<O3>' -S %t -o %t.o3
; RUN: lli %t.o3
; RUN: opt -verify-each -jeandle-loop-strip-mining-iter=2 -passes='loop-simplify,lcssa,safepoint-budget-strip-mining,safepoint-poll-elimination<after-strip-mining>,verify<jeandle-safepoint-coverage>' -S %s -o %t.two
; RUN: lli %t.two two

; Execute every boundary around several batches, both poll branches, and both
; natural exits. Volatile counters detect replay of preheader/header work and
; any movement of a poll across the store from its source iteration.
@expected.batch = global i32 4
@entries = global i32 0
@headers = global i32 0
@polls = global i32 0
@current = global i32 0
@sink = global i32 -1
@failure = global i1 false
@stride = global i32 1
@flag = global i1 false
@a = global i32 1
@b = global i32 2

define hotspotcc void @jeandle.safepoint_poll() "lower-phase"="1" {
  %i = load volatile i32, ptr @current
  %last = load volatile i32, ptr @sink
  %expected = add i32 %last, 1
  %bad = icmp ne i32 %i, %expected
  %failed = load volatile i1, ptr @failure
  %any = or i1 %bad, %failed
  store volatile i1 %any, ptr @failure
  %n = load volatile i32, ptr @polls
  %next = add i32 %n, 1
  store volatile i32 %next, ptr @polls
  ret void
}

define i32 @run(i32 %limit, i32 %stop) "java-method" {
entry:
  %entries = load volatile i32, ptr @entries
  %entries.next = add i32 %entries, 1
  store volatile i32 %entries.next, ptr @entries
  br label %header
header:
  %iv = phi i32 [0, %entry], [%next, %latch]
  %self = phi i32 [37, %entry], [%self, %latch]
  %left = phi ptr [@a, %entry], [%right, %latch]
  %right = phi ptr [@b, %entry], [%left, %latch]
  %headers = load volatile i32, ptr @headers
  %headers.next = add i32 %headers, 1
  store volatile i32 %headers.next, ptr @headers
  store volatile i32 %iv, ptr @current
  %done = icmp eq i32 %iv, %limit
  br i1 %done, label %exit, label %body
body:
  %early = icmp eq i32 %iv, %stop
  br i1 %early, label %exit, label %choose
choose:
  %flag = load volatile i1, ptr @flag
  br i1 %flag, label %poll.a, label %poll.b
poll.a:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 10, i32 %iv, ptr %left)]
  store volatile i32 %iv, ptr @sink
  br label %latch
poll.b:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 20, i32 %iv, ptr %right)]
  store volatile i32 %iv, ptr @sink
  br label %latch
latch:
  %step = load volatile i32, ptr @stride
  %next = add i32 %iv, %step
  br label %header
exit:
  %result = phi i32 [%iv, %header], [%iv, %body]
  %self.out = phi i32 [%self, %header], [%self, %body]
  %left.out = phi ptr [%left, %header], [%left, %body]
  %parity = and i32 %result, 1
  %even = icmp eq i32 %parity, 0
  %expected = select i1 %even, ptr @a, ptr @b
  %bad.oop = icmp ne ptr %left.out, %expected
  %bad.self = icmp ne i32 %self.out, 37
  %bad = or i1 %bad.oop, %bad.self
  %failed = load volatile i1, ptr @failure
  %any = or i1 %bad, %failed
  store volatile i1 %any, ptr @failure
  ret i32 %result
}

; CHECK-LABEL: define i32 @run(
; CHECK: batch.outer:
; CHECK: header.fast:
; CHECK: %batch.tick = phi i64
; CHECK: poll.b.fast:
; CHECK-NOT: call hotspotcc void @jeandle.safepoint_poll
; CHECK: store volatile i32 %iv.fast
; CHECK: poll.a.fast:
; CHECK-NOT: call hotspotcc void @jeandle.safepoint_poll
; CHECK: store volatile i32 %iv.fast
; CHECK: poll.a:
; CHECK: call hotspotcc void @jeandle.safepoint_poll() {{.*}}"deopt"(i32 10
; CHECK: poll.b:
; CHECK: call hotspotcc void @jeandle.safepoint_poll() {{.*}}"deopt"(i32 20

define i32 @check(i32 %k, i1 %flag, i1 %side) {
  store volatile i32 0, ptr @entries
  store volatile i32 0, ptr @headers
  store volatile i32 0, ptr @polls
  store volatile i32 -1, ptr @sink
  store volatile i1 false, ptr @failure
  store volatile i1 %flag, ptr @flag
  %longer = add i32 %k, 5
  %limit = select i1 %side, i32 %longer, i32 %k
  %stop = select i1 %side, i32 %k, i32 -1
  %result = call i32 @run(i32 %limit, i32 %stop)
  %entries = load volatile i32, ptr @entries
  %headers = load volatile i32, ptr @headers
  %polls = load volatile i32, ptr @polls
  %sink = load volatile i32, ptr @sink
  %failed = load volatile i1, ptr @failure
  %expected.headers = add i32 %k, 1
  %budget = load i32, ptr @expected.batch
  %expected.polls = udiv i32 %k, %budget
  %expected.sink = add i32 %k, -1
  %v0 = icmp ne i32 %result, %k
  %v1 = icmp ne i32 %entries, 1
  %v2 = icmp ne i32 %headers, %expected.headers
  %v3 = icmp ne i32 %polls, %expected.polls
  %v4 = icmp ne i32 %sink, %expected.sink
  %b0 = or i1 %v0, %v1
  %b1 = or i1 %v2, %v3
  %b2 = or i1 %v4, %failed
  %b3 = or i1 %b0, %b1
  %bad = or i1 %b3, %b2
  %status = zext i1 %bad to i32
  ret i32 %status
}

define i32 @main(i32 %argc, ptr %argv) {
entry:
  %two = icmp eq i32 %argc, 2
  %budget = select i1 %two, i32 2, i32 4
  store i32 %budget, ptr @expected.batch
  br label %loop
loop:
  %k = phi i32 [0, %entry], [%next, %loop]
  %failed = phi i32 [0, %entry], [%any3, %loop]
  %a = call i32 @check(i32 %k, i1 true, i1 false)
  %b = call i32 @check(i32 %k, i1 false, i1 false)
  %c = call i32 @check(i32 %k, i1 true, i1 true)
  %d = call i32 @check(i32 %k, i1 false, i1 true)
  %any0 = or i32 %failed, %a
  %any1 = or i32 %any0, %b
  %any2 = or i32 %any1, %c
  %any3 = or i32 %any2, %d
  %next = add i32 %k, 1
  %more = icmp ult i32 %next, 66
  br i1 %more, label %loop, label %exit
exit:
  ret i32 %any3
}

!java-method-compilation = !{}
