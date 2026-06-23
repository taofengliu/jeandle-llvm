; RUN: opt -S --jeandle --print-pipeline-passes %s 2>&1 | FileCheck %s
; RUN: opt -S --jeandle -jeandle-verify-safepoint-coverage=warn --print-pipeline-passes %s 2>&1 \
; RUN:   | FileCheck %s --check-prefix=VERIFY

; Keep this check loose: the exact Jeandle pre-pipeline cleanup order changes
; often. The important invariant is that safepoint-elimination is wired into
; --jeandle before phase-1 lowering can consume safepoint polls. The coverage
; verifier is wired in after it, but only when
; -jeandle-verify-safepoint-coverage is warn or fatal (not off).

; CHECK: safepoint-elimination{{.*}}java-operation-lower<phase=1>
; CHECK-NOT: verify<jeandle-safepoint-coverage>

; VERIFY: safepoint-elimination{{.*}}verify<jeandle-safepoint-coverage>{{.*}}java-operation-lower<phase=1>

define hotspotcc void @f() {
entry:
  ret void
}
