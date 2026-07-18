; RUN: opt -S --jeandle --print-pipeline-passes %s 2>&1 | FileCheck %s --check-prefix=PEA-ON
; RUN: opt -S --jeandle --jeandle-pea-iterations=0 --print-pipeline-passes %s 2>&1 | FileCheck %s --check-prefix=PEA-OFF

; The whole PEA segment — pre-PEA high-tier cluster, pre-PEA cleanup, PEA
; itself, and the post-PEA cleanup — is gated on -jeandle-pea-iterations > 0.

; PEA-ON: {{.*jeandle-inline-driver,function\(java-op-length-folding,loop-simplify,loop\(loop-rotate.*indvars,loop-deletion\),loop-unroll<no-partial;no-runtime;no-upperbound;full-unroll-max=128;O3>,loop\(simple-loop-unswitch<nontrivial;trivial>\),instcombine.*gvn.*simplifycfg.*\),function\(adce\),function\(simplifycfg.*\),function\(loop-simplify\),function\(instcombine.*\),function\(partial-escape-iterative\),function\(instsimplify\),function\(type-check-elimination\),function\(repeated-constant-field-folding\),function\(type-check-elimination\),function\(insert-gc-barriers\),java-operation-lower\<phase=1\>.*}}

; With PEA disabled the inline driver is immediately followed by
; insert-gc-barriers: no cluster, no cleanup, no PEA.
; PEA-OFF: {{.*jeandle-inline-driver,function\(insert-gc-barriers\),java-operation-lower\<phase=1\>.*}}

define hotspotcc void @pipeline_gate() {
entry:
  ret void
}
