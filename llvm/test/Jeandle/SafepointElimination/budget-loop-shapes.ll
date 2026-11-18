; RUN: opt -passes='loop-simplify,lcssa,safepoint-strip-mining,safepoint-budget-strip-mining,safepoint-poll-elimination<after-strip-mining>,verify<jeandle-safepoint-coverage>' -jeandle-loop-strip-mining-iter=4 -verify-each -S %s | FileCheck %s
; RUN: opt -passes='loop-simplify,lcssa,safepoint-budget-strip-mining,safepoint-poll-elimination<after-strip-mining>,loop(loop-rotate,indvars,loop-unroll-full),verify<jeandle-safepoint-coverage>' -jeandle-loop-strip-mining-iter=4 -verify-each -disable-output %s
; RUN: opt -passes='loop-simplify,lcssa,safepoint-budget-strip-mining,verify<jeandle-safepoint-coverage>' -jeandle-loop-strip-mining-iter=18446744073709551615 -verify-each -S %s | FileCheck %s --check-prefix=MAX

declare hotspotcc void @jeandle.safepoint_poll()

; The original i32 update remains modular. Both exits keep the source values,
; including a current-IV exit before the poll. Swapping oop recurrences must
; remain simultaneous across the batch boundary.
define i32 @irregular(ptr %step.addr, ptr %out, i32 %start, i32 %limit,
                      ptr %first, ptr %second) "java-method" {
entry:
  br label %head
head:
  %iv = phi i32 [ %start, %entry ], [ %next, %tail ]
  %a = phi ptr [ %first, %entry ], [ %b, %tail ]
  %b = phi ptr [ %second, %entry ], [ %a, %tail ]
  %stop = icmp eq i32 %iv, 17
  br i1 %stop, label %side, label %tail
tail:
  %step = load volatile i32, ptr %step.addr
  %next = add i32 %iv, %step
  store i32 %next, ptr %out
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i32 %iv, i32 %next, ptr %a, ptr %b) ]
  %keep = icmp slt i32 %iv, %limit
  br i1 %keep, label %head, label %exit
side:
  ret i32 %iv
exit:
  ret i32 %next
}

; CHECK-LABEL: @irregular(
; CHECK: head.batch:
; CHECK: %a.batch = phi ptr [ %first, %entry ], [ %b.batch.state, %head.batch.poll ]
; CHECK: %b.batch = phi ptr [ %second, %entry ], [ %a.batch.state, %head.batch.poll ]
; CHECK: %stop = icmp eq i32 %iv, 17
; CHECK: br i1 %stop, label %side, label %tail
; CHECK: %next = add i32 %iv, %step
; CHECK-NOT: @jeandle.safepoint_poll
; CHECK: %keep = icmp slt i32 %iv, %limit
; CHECK: br i1 %keep, label %head.batch.check, label %exit
; CHECK: side:
; CHECK: ret i32 %{{.*}}
; CHECK: exit:
; CHECK: ret i32 %{{.*}}
; CHECK: head.batch.poll:
; CHECK-DAG: %iv.batch.state = phi i32 [ %iv, %head.batch.check ]
; CHECK-DAG: %a.batch.state = phi ptr [ %a, %head.batch.check ]
; CHECK-DAG: %b.batch.state = phi ptr [ %b, %head.batch.check ]
; CHECK: call hotspotcc void @jeandle.safepoint_poll() #{{[0-9]+}} [ "deopt"(i32 %iv.batch.state, i32 %next.batch.state, ptr %a.batch.state, ptr %b.batch.state) ]
; MAX-LABEL: @irregular(
; MAX: %batch.tick.next = add nuw i64 %batch.tick, 1
; MAX: icmp ult i64 %batch.tick.next, -1

; Applying a sibling transformation must not query the previous MemorySSA.
define void @siblings(ptr %step.addr, ptr %out) "java-method" {
entry:
  br label %one
one:
  %i = phi i32 [ 0, %entry ], [ %next.i, %one ]
  %step.i = load volatile i32, ptr %step.addr
  %next.i = add i32 %i, %step.i
  store i32 %next.i, ptr %out
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i32 %i) ]
  %keep.i = icmp ne i32 %next.i, 42
  br i1 %keep.i, label %one, label %between
between:
  br label %two
two:
  %j = phi i32 [ 0, %between ], [ %next.j, %two ]
  %step.j = load volatile i32, ptr %step.addr
  %next.j = add i32 %j, %step.j
  store i32 %next.j, ptr %out
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i32 %j) ]
  %keep.j = icmp ne i32 %next.j, 42
  br i1 %keep.j, label %two, label %exit
exit:
  ret void
}

; CHECK-LABEL: @siblings(
; CHECK: one.batch:
; CHECK: two.batch:
; CHECK: one.batch.poll:
; CHECK: call hotspotcc void @jeandle.safepoint_poll()
; CHECK: two.batch.poll:
; CHECK: call hotspotcc void @jeandle.safepoint_poll()
; MAX-LABEL: @siblings(
; MAX: %batch.tick.next = add nuw i64 %batch.tick, 1
; MAX: icmp ult i64 %batch.tick.next, -1
; MAX: %batch.tick.next{{[0-9]+}} = add nuw i64

!java-method-compilation = !{}
