; RUN: opt -passes='loop-simplify,lcssa,safepoint-budget-strip-mining,verify<jeandle-safepoint-coverage>' -jeandle-safepoint-max-work=0 -jeandle-loop-strip-mining-iter=4 -verify-each -S %s | FileCheck %s --check-prefix=OFF
; RUN: opt -passes='loop-simplify,lcssa,safepoint-budget-strip-mining,safepoint-poll-elimination<after-strip-mining>,verify<jeandle-safepoint-coverage>' -jeandle-loop-strip-mining-iter=4 -jeandle-verify-safepoint-coverage=fatal -verify-each -S %s | FileCheck %s
; RUN: opt -passes='loop-simplify,lcssa,safepoint-budget-strip-mining' -jeandle-loop-strip-mining-iter=0 -verify-each -S %s | FileCheck %s --check-prefix=OFF
; RUN: opt -passes='loop-simplify,lcssa,safepoint-budget-strip-mining' -jeandle-loop-strip-mining-iter=1 -verify-each -S %s | FileCheck %s --check-prefix=OFF
; RUN: opt -passes='loop-simplify,lcssa,safepoint-budget-strip-mining' -jeandle-enable-safepoint-elim=false -jeandle-loop-strip-mining-iter=4 -verify-each -S %s | FileCheck %s --check-prefix=OFF

; An independent counter bounds batches without altering the source condition
; or assuming a widened i32 root cannot wrap.
declare hotspotcc void @jeandle.safepoint_poll()

define i32 @widened_root(ptr %out, i32 %start, i64 %limit) "java-method" {
entry:
  br label %header
header:
  %iv = phi i32 [%start, %entry], [%next, %body]
  %wide = sext i32 %iv to i64
  %cond = icmp slt i64 %wide, %limit
  br i1 %cond, label %body, label %exit
body:
  store i32 %iv, ptr %out
  %next = add i32 %iv, 1
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 %iv, i32 %next)]
  br label %header
exit:
  ret i32 %iv
}

; CHECK-LABEL: @widened_root(
; CHECK: header:
; CHECK: %[[TICK:[^ ]+]] = phi i64
; CHECK: %wide = sext i32 %iv to i64
; CHECK: %cond = icmp slt i64 %wide, %limit
; CHECK: %next = add i32 %iv, 1
; CHECK-NOT: call hotspotcc void @jeandle.safepoint_poll
; CHECK: header.batch.check:
; CHECK: %[[TICKNEXT:[^ ]+]] = add nuw nsw i64 %[[TICK]], 1
; CHECK: icmp ult i64 %[[TICKNEXT]], 4
; CHECK: header.batch.poll:
; CHECK: %[[CURRENT:[^ ]+]] = phi i32 [ %iv, %header.batch.check ]
; CHECK: call hotspotcc void @jeandle.safepoint_poll() {{.*}}[ "deopt"(i32 %[[CURRENT]], i32 %{{[^ )]+}}) ]
; OFF-LABEL: @widened_root(
; OFF-NOT: .batch
; OFF: call hotspotcc void @jeandle.safepoint_poll()

define void @poll_before_store(ptr %out, ptr %flag) "java-method" {
entry:
  br label %header
header:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"()]
  store i32 1, ptr %out
  %keep = load volatile i1, ptr %flag
  br i1 %keep, label %header, label %exit
exit:
  ret void
}

; CHECK-LABEL: @poll_before_store(
; CHECK: batch.outer:
; CHECK: header.fast:
; CHECK-NOT: call hotspotcc void @jeandle.safepoint_poll
; CHECK: store i32 1
; CHECK: header:
; CHECK: call hotspotcc void @jeandle.safepoint_poll()
; CHECK-NEXT: store i32 1
; OFF-LABEL: @poll_before_store(
; OFF-NOT: .batch
; OFF: call hotspotcc void @jeandle.safepoint_poll()

!java-method-compilation = !{}
