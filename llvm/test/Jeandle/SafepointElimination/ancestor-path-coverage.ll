; RUN: opt -passes='loop-simplify,lcssa,safepoint-budget-strip-mining,safepoint-poll-elimination<after-strip-mining>,verify<jeandle-safepoint-coverage>' -jeandle-safepoint-max-depth=1 -jeandle-loop-strip-mining-iter=2 -verify-each -S %s | FileCheck %s
; RUN: opt -passes='loop-simplify,lcssa,verify<jeandle-safepoint-coverage>,safepoint-poll-elimination<loop-deletion-prep>,verify<jeandle-safepoint-coverage>' -jeandle-verify-safepoint-coverage=fatal -verify-each -S %s | FileCheck %s
; RUN: opt -passes='loop-simplify,lcssa,safepoint-poll-elimination<after-strip-mining>,verify<jeandle-safepoint-coverage>' -jeandle-verify-safepoint-coverage=fatal -verify-each -S %s | FileCheck %s
; RUN: opt -passes='loop-simplify,lcssa,safepoint-budget-strip-mining,safepoint-poll-elimination<after-strip-mining>,verify<jeandle-safepoint-coverage>' -jeandle-loop-strip-mining-iter=2 -jeandle-verify-safepoint-coverage=fatal -verify-each -S %s | FileCheck %s

; CHECK-LABEL: @branch_descendant(
; CHECK-NOT: .fast
; CHECK: inner.header:
; CHECK: call hotspotcc void @jeandle.safepoint_poll()
; CHECK: call.arm:
; CHECK: call hotspotcc void @java_method()

; A branch-arm descendant poll and a call jointly cover an outer loop.
; Atomic empty-loop deletion must retain the descendant when its removal
; would uncover that outer backedge.
declare hotspotcc void @jeandle.safepoint_poll()
declare hotspotcc void @java_method()
define void @branch_descendant(ptr %flag, ptr %keep_going) "java-method" {
entry:
  br label %outer.header
outer.header:
  %choose = load volatile i1, ptr %flag
  br i1 %choose, label %inner.preheader, label %call.arm
inner.preheader:
  br label %inner.header
inner.header:
  %iv = phi i32 [0, %inner.preheader], [%next, %inner.header]
  %next = add nuw nsw i32 %iv, 1
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 %next)]
  %continue.inner = icmp ult i32 %next, 4
  br i1 %continue.inner, label %inner.header, label %inner.exit
inner.exit:
  br label %outer.latch
call.arm:
  call hotspotcc void @java_method() ["deopt"()]
  br label %outer.latch
outer.latch:
  %continue.outer = load volatile i1, ptr %keep_going
  br i1 %continue.outer, label %outer.header, label %exit
exit:
  ret void
}
!java-method-compilation = !{}
