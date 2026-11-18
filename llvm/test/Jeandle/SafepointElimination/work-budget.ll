; RUN: opt -passes='loop-simplify,lcssa,safepoint-budget-strip-mining,verify<jeandle-safepoint-coverage>' -jeandle-loop-strip-mining-iter=4 -verify-each -S %s | FileCheck %s --check-prefix=ALL
; RUN: opt -passes='loop-simplify,lcssa,safepoint-budget-strip-mining,verify<jeandle-safepoint-coverage>' -jeandle-safepoint-max-work=75 -jeandle-loop-strip-mining-iter=4 -verify-each -S %s | FileCheck %s --check-prefix=LIMIT

; Rebuilding analyses after the first sibling does not replenish the shared
; allowance. The second loop keeps its source poll when work runs out.
; ALL-COUNT-2: .batch.poll:
; LIMIT: second:
; LIMIT: call hotspotcc void @jeandle.safepoint_poll()
; LIMIT-COUNT-1: .batch.poll:
; LIMIT-NOT: .batch.poll:

declare hotspotcc void @jeandle.safepoint_poll()
define void @siblings(ptr %flag, ptr %out) "java-method" {
entry:
  br label %first
first:
  %a = load volatile i1, ptr %flag
  store i32 1, ptr %out
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"()]
  br i1 %a, label %first, label %between
between:
  br label %second
second:
  %b = load volatile i1, ptr %flag
  store i32 2, ptr %out
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"()]
  br i1 %b, label %second, label %exit
exit:
  ret void
}
!java-method-compilation = !{}
