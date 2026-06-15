; RUN: opt -S --jeandle --print-pipeline-passes %s 2>&1 | FileCheck %s
; RUN: opt -S --jeandle -jeandle-verify-safepoint-coverage --print-pipeline-passes %s 2>&1 \
; RUN:   | FileCheck %s --check-prefix=VERIFY

; safepoint-elimination must sit in the Jeandle pipeline after
; type-check-elimination and before the default pipeline (whose tail here is
; java-operation-lower<phase=1>). The coverage verifier is wired in right after
; it, but only when -jeandle-verify-safepoint-coverage is set.

; CHECK: type-check-elimination{{.*}}safepoint-elimination{{.*}}java-operation-lower<phase=1>
; CHECK-NOT: verify<jeandle-safepoint-coverage>

; VERIFY: safepoint-elimination{{.*}}verify<jeandle-safepoint-coverage>{{.*}}java-operation-lower<phase=1>

define hotspotcc void @f() {
entry:
  ret void
}
