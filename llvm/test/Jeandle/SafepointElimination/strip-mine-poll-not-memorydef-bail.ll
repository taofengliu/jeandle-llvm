; RUN: opt -passes='safepoint-elimination<early>,safepoint-elimination<strip-mining>' \
; RUN:   -jeandle-enable-strip-mining -S < %s | FileCheck %s

; A poll that is not a MemoryDef cannot be proven equal to the backedge memory
; state. The transform must fail closed even though no write follows it.

declare hotspotcc void @jeandle.safepoint_poll() memory(read)

define void @poll_memory_use_bails(i64 %n) {
entry:
  br label %header
header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %latch ]
  %cond = icmp slt i64 %iv, %n
  br i1 %cond, label %body, label %exit
body:
  %iv.next = add i64 %iv, 1
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
  br label %latch
latch:
  br label %header
exit:
  ret void
}

; CHECK-LABEL: @poll_memory_use_bails(
; CHECK-NOT:   .outer
; CHECK:       call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]

!java-method-compilation = !{}
