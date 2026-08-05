; RUN: opt -S --jeandle --print-pipeline-passes %s 2>&1 | FileCheck %s

; CHECK: java-operation-lower<phase=0>
; CHECK-SAME: jeandle-inline-driver
; CHECK-SAME: constant-field-folding
; CHECK-SAME: safepoint-poll-elimination
; CHECK-SAME: insert-gc-barriers
; CHECK-SAME: safepoint-poll-elimination
; CHECK-SAME: java-operation-lower<phase=1>
; CHECK-SAME: expand-narrow-oop-cast
; CHECK-SAME: rewrite-statepoints-for-gc
; CHECK-SAME: jeandle-narrow-oop-marker
; CHECK-SAME: java-operation-lower<phase=9>
; CHECK-SAME: java-operation-deletion
; CHECK-SAME: tls-pointer-rewrite
; CHECK-SAME: instsimplify

define hotspotcc void @opt_option() {
entry:
  ret void
}
