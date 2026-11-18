; RUN: opt -verify-each -jeandle-loop-strip-mining-iter=4 -passes='loop-simplify,lcssa,safepoint-budget-strip-mining' -S %s | FileCheck %s
; RUN: opt -verify-each -jeandle-loop-strip-mining-iter=4 -passes='loop-simplify,lcssa,safepoint-budget-strip-mining,safepoint-poll-elimination<loop-deletion-prep>,verify<jeandle-safepoint-coverage>' -S %s | FileCheck %s --check-prefix=DELETED

; Leave an empty finite region for atomic deletion even when running the
; cloning pass independently of the production pipeline's deferral flag.
declare hotspotcc void @jeandle.safepoint_poll()
define void @empty_split(i1 %choose) "java-method" {
entry: br label %header
header:
  %iv = phi i32 [0, %entry], [%next, %latch]
  br i1 %choose, label %left, label %right
left:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 10, i32 %iv)]
  br label %latch
right:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 20, i32 %iv)]
  br label %latch
latch:
  %next = add nuw nsw i32 %iv, 1
  %more = icmp ult i32 %next, 10000
  br i1 %more, label %header, label %exit
exit: ret void
}
; CHECK-LABEL: define void @empty_split(
; CHECK-NOT: batch.outer
; CHECK-NOT: .fast
; CHECK: header:
; CHECK: call hotspotcc void @jeandle.safepoint_poll() {{.*}}"deopt"(i32 10
; CHECK: call hotspotcc void @jeandle.safepoint_poll() {{.*}}"deopt"(i32 20
; CHECK: ret void
; DELETED-LABEL: define void @empty_split(
; DELETED-NOT: call hotspotcc void @jeandle.safepoint_poll
; DELETED-NOT: phi i32
; DELETED: ret void
!java-method-compilation = !{}
