; RUN: opt -S --jeandle -jeandle-enable-strip-mining --print-pipeline-passes %s 2>&1 \
; RUN:   | FileCheck %s --implicit-check-not='verify<jeandle-safepoint-coverage>'
; RUN: opt -S --jeandle -jeandle-enable-strip-mining \
; RUN:   -jeandle-verify-safepoint-coverage=warn --print-pipeline-passes %s 2>&1 \
; RUN:   | FileCheck %s --check-prefix=VERIFY

; Keep this check loose: the important invariant is the three-pass shape without
; changing the established lowering/default-pipeline order. Early and
; StripMining run as separate invocations before barrier insertion, and Cleanup
; runs before phase-1 lowering consumes jeandle.safepoint_poll calls. The
; coverage verifier is wired after each pass, but only when
; -jeandle-verify-safepoint-coverage is warn or fatal (not off).

; CHECK:      safepoint-elimination
; CHECK-SAME: safepoint-elimination
; CHECK-SAME: insert-gc-barriers
; CHECK-SAME: safepoint-elimination
; CHECK-SAME: java-operation-lower<phase=1>
; CHECK-SAME: expand-narrow-oop-cast
; CHECK-SAME: rewrite-statepoints-for-gc
; CHECK-SAME: jeandle-narrow-oop-marker
; CHECK-SAME: java-operation-lower<phase=9>
; CHECK-SAME: java-operation-deletion
; CHECK-SAME: tls-pointer-rewrite
; VERIFY:      safepoint-elimination
; VERIFY-SAME: verify<jeandle-safepoint-coverage>
; VERIFY-SAME: safepoint-elimination
; VERIFY-SAME: verify<jeandle-safepoint-coverage>
; VERIFY-SAME: insert-gc-barriers
; VERIFY-SAME: safepoint-elimination
; VERIFY-SAME: verify<jeandle-safepoint-coverage>
; VERIFY-SAME: java-operation-lower<phase=1>
; VERIFY-SAME: expand-narrow-oop-cast
; VERIFY-SAME: rewrite-statepoints-for-gc
; VERIFY-SAME: jeandle-narrow-oop-marker
; VERIFY-SAME: java-operation-lower<phase=9>
; VERIFY-SAME: java-operation-deletion
; VERIFY-SAME: tls-pointer-rewrite

define hotspotcc void @f() {
entry:
  ret void
}
