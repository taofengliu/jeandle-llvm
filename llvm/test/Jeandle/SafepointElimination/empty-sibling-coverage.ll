; RUN: opt -verify-each -jeandle-verify-safepoint-coverage=fatal -passes='loop-simplify,lcssa,safepoint-poll-elimination<loop-deletion-prep>,verify<jeandle-safepoint-coverage>' -S %s | FileCheck %s
; RUN: opt -verify-each -jeandle-verify-safepoint-coverage=fatal -passes='loop-simplify,lcssa,safepoint-poll-elimination<loop-deletion-prep>,safepoint-poll-elimination<loop-deletion-prep>,verify<jeandle-safepoint-coverage>' -S %s | FileCheck %s
; RUN: opt -verify-each -jeandle-verify-safepoint-coverage=fatal -passes='loop-simplify,lcssa,safepoint-poll-elimination<after-strip-mining;defer-empty-loop-deletion>,safepoint-poll-elimination<loop-deletion-prep>,verify<jeandle-safepoint-coverage>' -S %s | FileCheck %s

; Sequential empty siblings may cover their common unbounded ancestor. Each
; committed deletion must recheck dependencies before deleting another region.
; CHECK-LABEL: define void @siblings_without_outer_poll
; CHECK: call void @jeandle.safepoint_poll()
; CHECK-NOT: call void @jeandle.safepoint_poll()
; CHECK: ret void
; CHECK-LABEL: define void @siblings_with_outer_poll
; CHECK: call void @jeandle.safepoint_poll()
; CHECK-NOT: call void @jeandle.safepoint_poll()
; CHECK-NOT: phi i32
; CHECK: ret void

declare void @jeandle.safepoint_poll()
define void @siblings_without_outer_poll(ptr %out, ptr %flag) "java-method" {
entry:
  br label %outer
outer:
  store volatile i32 1, ptr %out
  br label %pre1
pre1:
  br label %inner1
inner1:
  %i = phi i32 [0, %pre1], [%in, %inner1]
  call void @jeandle.safepoint_poll() ["deopt"(i32 %i)]
  %in = add nuw nsw i32 %i, 1
  %ic = icmp ult i32 %in, 4
  br i1 %ic, label %inner1, label %exit1
exit1:
  br label %pre2
pre2:
  br label %inner2
inner2:
  %j = phi i32 [0, %pre2], [%jn, %inner2]
  call void @jeandle.safepoint_poll() ["deopt"(i32 %j)]
  %jn = add nuw nsw i32 %j, 1
  %jc = icmp ult i32 %jn, 4
  br i1 %jc, label %inner2, label %exit2
exit2:
  br label %latch
latch:
  %more = load volatile i1, ptr %flag
  br i1 %more, label %outer, label %exit
exit:
  ret void
}

define void @siblings_with_outer_poll(ptr %out, ptr %flag) "java-method" {
entry:
  br label %outer
outer:
  call void @jeandle.safepoint_poll() ["deopt"(i32 42)]
  store volatile i32 1, ptr %out
  br label %pre1
pre1:
  br label %inner1
inner1:
  %i = phi i32 [0, %pre1], [%in, %inner1]
  call void @jeandle.safepoint_poll() ["deopt"(i32 %i)]
  %in = add nuw nsw i32 %i, 1
  %ic = icmp ult i32 %in, 4
  br i1 %ic, label %inner1, label %exit1
exit1:
  br label %pre2
pre2:
  br label %inner2
inner2:
  %j = phi i32 [0, %pre2], [%jn, %inner2]
  call void @jeandle.safepoint_poll() ["deopt"(i32 %j)]
  %jn = add nuw nsw i32 %j, 1
  %jc = icmp ult i32 %jn, 4
  br i1 %jc, label %inner2, label %exit2
exit2:
  br label %latch
latch:
  %more = load volatile i1, ptr %flag
  br i1 %more, label %outer, label %exit
exit:
  ret void
}

!java-method-compilation = !{}
