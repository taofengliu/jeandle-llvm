; RUN: opt -S --jeandle -jeandle-enable-strip-mining \
; RUN:   -jeandle-enable-inclusive-loop-versioning --print-pipeline-passes %s 2>&1 \
; RUN:   | FileCheck %s --implicit-check-not='verify<jeandle-safepoint-coverage>'
; RUN: opt -S --jeandle -jeandle-enable-strip-mining \
; RUN:   -jeandle-enable-inclusive-loop-versioning \
; RUN:   -jeandle-verify-safepoint-coverage=warn --print-pipeline-passes %s 2>&1 \
; RUN:   | FileCheck %s --check-prefix=VERIFY
; RUN: opt -S --jeandle -jeandle-enable-strip-mining \
; RUN:   -jeandle-enable-inclusive-loop-versioning=false \
; RUN:   --print-pipeline-passes %s 2>&1 \
; RUN:   | FileCheck %s --check-prefix=NO-INCLUSIVE
; RUN: opt -S --jeandle -jeandle-enable-strip-mining=false \
; RUN:   --print-pipeline-passes %s 2>&1 \
; RUN:   | FileCheck %s --check-prefix=NO-STRIP
; RUN: opt -S -passes='jeandle<O0>' -jeandle-enable-strip-mining=false \
; RUN:   --print-pipeline-passes %s 2>&1 \
; RUN:   | FileCheck %s --check-prefix=O0

; Keep this check loose: the important invariant is the five-mode shape without
; changing the established lowering/default-pipeline order. Early runs before
; the post-elimination canonicalization. InclusiveLoopVersioning and
; StripMining run after SimplifyCFG, LoopSimplify/LCSSA, and rotate/LICM expose
; their validated input shape. Cleanup runs before phase-1 lowering consumes
; safepoint poll calls.
; At O3, LCSSA and atomic empty-loop deletion run after the final coverage
; verifier. The verifier is wired after poll-removing phases when the check is
; warn or fatal (not off).

; CHECK:      safepoint-elimination<early>
; CHECK-SAME: function(simplifycfg
; CHECK-SAME: function(loop-simplify),function(lcssa)
; CHECK-SAME: loop-mssa(loop-rotate<header-duplication;no-prepare-for-lto>,licm<allowspeculation>)
; CHECK-SAME: safepoint-elimination<inclusive-loop-versioning>
; CHECK-SAME: safepoint-elimination<strip-mining>
; CHECK-SAME: insert-gc-barriers
; CHECK-SAME: safepoint-elimination<cleanup>
; CHECK-SAME: function(lcssa)
; CHECK-SAME: safepoint-elimination<loop-deletion-prep>
; CHECK-SAME: java-operation-lower<phase=1>
; CHECK-SAME: expand-narrow-oop-cast
; CHECK-SAME: rewrite-statepoints-for-gc
; CHECK-SAME: jeandle-narrow-oop-marker
; CHECK-SAME: java-operation-lower<phase=9>
; CHECK-SAME: java-operation-deletion
; CHECK-SAME: tls-pointer-rewrite
; VERIFY:      safepoint-elimination<early>
; VERIFY-SAME: verify<jeandle-safepoint-coverage>
; VERIFY-SAME: function(simplifycfg
; VERIFY-SAME: function(loop-simplify),function(lcssa)
; VERIFY-SAME: loop-mssa(loop-rotate<header-duplication;no-prepare-for-lto>,licm<allowspeculation>)
; VERIFY-SAME: safepoint-elimination<inclusive-loop-versioning>
; VERIFY-SAME: safepoint-elimination<strip-mining>
; VERIFY-SAME: verify<jeandle-safepoint-coverage>
; VERIFY-SAME: insert-gc-barriers
; VERIFY-SAME: safepoint-elimination<cleanup>
; VERIFY-SAME: verify<jeandle-safepoint-coverage>
; VERIFY-SAME: function(lcssa)
; VERIFY-SAME: safepoint-elimination<loop-deletion-prep>
; VERIFY-SAME: java-operation-lower<phase=1>
; VERIFY-SAME: expand-narrow-oop-cast
; VERIFY-SAME: rewrite-statepoints-for-gc
; VERIFY-SAME: jeandle-narrow-oop-marker
; VERIFY-SAME: java-operation-lower<phase=9>
; VERIFY-SAME: java-operation-deletion
; VERIFY-SAME: tls-pointer-rewrite
; NO-INCLUSIVE:      safepoint-elimination<early>
; NO-INCLUSIVE-SAME: function(simplifycfg
; NO-INCLUSIVE-SAME: function(loop-simplify),function(lcssa)
; NO-INCLUSIVE-SAME: loop-mssa(loop-rotate<header-duplication;no-prepare-for-lto>,licm<allowspeculation>)
; NO-INCLUSIVE-NOT:  safepoint-elimination<inclusive-loop-versioning>
; NO-INCLUSIVE-SAME: safepoint-elimination<strip-mining>
; NO-INCLUSIVE-SAME: insert-gc-barriers
; NO-INCLUSIVE-SAME: safepoint-elimination<cleanup>
; NO-INCLUSIVE-SAME: function(lcssa)
; NO-INCLUSIVE-SAME: safepoint-elimination<loop-deletion-prep>
; NO-INCLUSIVE-SAME: java-operation-lower<phase=1>
; NO-STRIP:      safepoint-elimination<early>
; NO-STRIP-NOT:  function(simplifycfg
; NO-STRIP-NOT:  function(loop-simplify)
; NO-STRIP-NOT:  function(lcssa)
; NO-STRIP-NOT:  loop-mssa(
; NO-STRIP-NOT:  safepoint-elimination<inclusive-loop-versioning>
; NO-STRIP-NOT:  safepoint-elimination<strip-mining>
; NO-STRIP-SAME: insert-gc-barriers
; NO-STRIP-SAME: safepoint-elimination<cleanup>
; NO-STRIP-SAME: function(lcssa)
; NO-STRIP-SAME: safepoint-elimination<loop-deletion-prep>
; NO-STRIP-SAME: java-operation-lower<phase=1>
; O0: function(safepoint-elimination<early>)
; O0-NOT: function(loop-simplify)
; O0-NOT: function(lcssa)
; O0-NOT: loop-mssa(
; O0-NOT: safepoint-elimination<inclusive-loop-versioning>
; O0-NOT: safepoint-elimination<strip-mining>
; O0-SAME: insert-gc-barriers
; O0-SAME: safepoint-elimination<cleanup>
; O0-NOT: function(lcssa)
; O0-NOT: safepoint-elimination<loop-deletion-prep>
; O0-SAME: java-operation-lower<phase=1>

define hotspotcc void @f() {
entry:
  ret void
}
