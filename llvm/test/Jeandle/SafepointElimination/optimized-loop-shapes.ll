; RUN: opt -verify-each -jeandle-loop-strip-mining-iter=4 -passes='loop-simplify,lcssa,safepoint-strip-mining,safepoint-budget-strip-mining,safepoint-poll-elimination<after-strip-mining>,verify<jeandle-safepoint-coverage>' -S %s | FileCheck %s --check-prefix=CLONE
; RUN: opt -verify-each -jeandle-loop-strip-mining-iter=4 -passes='loop-simplify,lcssa,loop-mssa(simple-loop-unswitch<nontrivial;trivial>),safepoint-strip-mining,safepoint-budget-strip-mining,safepoint-poll-elimination<after-strip-mining>,verify<jeandle-safepoint-coverage>' -S %s | FileCheck %s --check-prefix=UNSWITCH
; RUN: opt -verify-each -jeandle-loop-strip-mining-iter=4 -unroll-peel-count=1 -passes='loop-simplify,lcssa,safepoint-strip-mining,safepoint-budget-strip-mining,safepoint-poll-elimination<after-strip-mining>,loop-unroll,verify<jeandle-safepoint-coverage>' -S %s | FileCheck %s --check-prefix=PEEL
; RUN: opt -verify-each -jeandle-loop-strip-mining-iter=4 -passes='function(loop-simplify,lcssa,safepoint-strip-mining,safepoint-budget-strip-mining,safepoint-poll-elimination<after-strip-mining>,verify<jeandle-safepoint-coverage>),java-operation-lower<phase=1>,default<O3>' -S %s | FileCheck %s --check-prefix=LOWERED --implicit-check-not="call hotspotcc void @jeandle.safepoint_poll"

; Unswitching before safepoint optimization creates single-poll loops and can
; use relocation. Without unswitching the distinct states use one fast clone.
; Peeling must really occur, and expanded polls still participate in O3 at the
; existing phase-1 position. Coverage verification ends before expansion.

@pollword = external thread_local global i64

declare void @poll_handler()
define hotspotcc void @jeandle.safepoint_poll() noinline "lower-phase"="1" {
entry:
  %word = load volatile i64, ptr @pollword
  %request = icmp ne i64 %word, 0
  br i1 %request, label %slow, label %exit
slow:
  call void @poll_handler() ["deopt"()]
  br label %exit
exit: ret void
}
define void @split(ptr %out, i64 noundef %n, i1 %choose) "java-method" {
entry: br label %loop
loop:
  %iv = phi i64 [0, %entry], [%next, %latch]
  %next = add nsw i64 %iv, 1
  br i1 %choose, label %left, label %right
left:
  store i64 %next, ptr %out
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 10, i64 %next)]
  br label %latch
right:
  store i64 %iv, ptr %out
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 20, i64 %next)]
  br label %latch
latch:
  %keep = icmp slt i64 %next, %n
  br i1 %keep, label %loop, label %exit
exit: ret void
}

!java-method-compilation = !{}

; CLONE-LABEL: define void @split(
; CLONE: batch.outer:
; CLONE: loop.fast:
; CLONE-NOT: call hotspotcc void @jeandle.safepoint_poll
; CLONE: loop:
; CLONE: call hotspotcc void @jeandle.safepoint_poll() {{.*}}"deopt"(i32 10
; CLONE: call hotspotcc void @jeandle.safepoint_poll() {{.*}}"deopt"(i32 20
; CLONE: batch.check:
; CLONE: icmp ult i64 %batch.tick.next, 3
; UNSWITCH-LABEL: define void @split(
; UNSWITCH: .us
; UNSWITCH: call hotspotcc void @jeandle.safepoint_poll() #{{[0-9]+}}
; UNSWITCH: call hotspotcc void @jeandle.safepoint_poll() #{{[0-9]+}}
; UNSWITCH: attributes #{{[0-9]+}} = { "jeandle.strip-mined-poll" }
; PEEL-LABEL: define void @split(
; PEEL: .peel
; PEEL: call hotspotcc void @jeandle.safepoint_poll()
; LOWERED-LABEL: define void @split(
; LOWERED-NOT: call hotspotcc void @jeandle.safepoint_poll
; LOWERED-DAG: load volatile i64
; LOWERED-DAG: call void @poll_handler() {{.*}}"deopt"(i32 10
; LOWERED-DAG: load volatile i64
; LOWERED-DAG: call void @poll_handler() {{.*}}"deopt"(i32 20
; LOWERED: }
