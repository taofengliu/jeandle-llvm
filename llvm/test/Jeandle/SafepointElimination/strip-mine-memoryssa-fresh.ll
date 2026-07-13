; RUN: opt -passes='require<memoryssa>,safepoint-elimination<early>,safepoint-elimination<strip-mining>,verify<memoryssa>,verify' \
; RUN:   -jeandle-enable-strip-mining -verify-analysis-invalidation \
; RUN:   -verify-memoryssa -S < %s | FileCheck %s

; Early removes one adjacent poll after MemorySSA has already been cached.
; StripMining must request a rebuilt analysis rather than observe the stale
; MemoryDef chain.

declare hotspotcc void @jeandle.safepoint_poll()

define void @fresh_memoryssa_after_early_mutation(i64 %n) {
entry:
  br label %header
header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %latch ]
  %cond = icmp slt i64 %iv, %n
  br i1 %cond, label %body, label %exit
body:
  %iv.next = add i64 %iv, 1
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i64 %iv.next) ]
  br label %latch
latch:
  br label %header
exit:
  ret void
}

; CHECK-LABEL: @fresh_memoryssa_after_early_mutation(
; CHECK:       header.outer:
; CHECK-COUNT-1: call hotspotcc void @jeandle.safepoint_poll(){{.*}}!poll-coverage

!java-method-compilation = !{}
