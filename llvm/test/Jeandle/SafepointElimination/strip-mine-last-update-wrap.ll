; RUN: opt -passes='loop-simplify,lcssa,loop(indvars),safepoint-strip-mining,safepoint-budget-strip-mining,safepoint-poll-elimination<after-strip-mining>,verify<jeandle-safepoint-coverage>' -jeandle-loop-strip-mining-iter=4 -verify-each -S %s | FileCheck %s

; The header recurrence never signed-wraps, but the last side-exiting update
; legally produces INT_MIN and stores it. Preserve that modular update.
; CHECK-LABEL: @last_update_overflow(
; CHECK: %next = add nuw i32 %iv, 2
; CHECK-NEXT: store volatile i32 %next, ptr %out
; CHECK: .batch.poll:

declare hotspotcc void @jeandle.safepoint_poll()
define i32 @last_update_overflow(i32 noundef %limit, ptr %out) "java-method" {
entry:
  %entry.cmp = icmp sgt i32 %limit, 0
  br i1 %entry.cmp, label %pre, label %done
pre:
  br label %loop
loop:
  %iv = phi i32 [0, %pre], [%next, %latch]
  %next = add i32 %iv, 2
  store volatile i32 %next, ptr %out
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 %next)]
  %last = icmp eq i32 %iv, 2147483646
  br i1 %last, label %wrap.exit, label %latch
latch:
  %more = icmp slt i32 %next, %limit
  br i1 %more, label %loop, label %exit
wrap.exit:
  %wrap.value = phi i32 [ %next, %loop ]
  ret i32 %wrap.value
exit:
  %value = phi i32 [ %next, %latch ]
  ret i32 %value
done:
  ret i32 0
}
!java-method-compilation = !{}
