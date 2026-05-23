; RUN: opt -S -passes="loop-simplify,require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; A1 — pessimistic fallback for a loop that processLoop encounters
; before LoopSimplify has canonicalised. We force the pipeline to run
; loop-simplify FIRST in this test so that PEA itself sees a proper
; preheader, but the residual A1 invariant is that processLoop's "no
; preheader" branch falls through to the legacy behaviour without
; crashing. A self-test of that path is exercised today via the bail
; case in materializeBeforeLoops. This test simply validates the
; standard loop-with-preheader path still works under A1.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_simple_loop(i32 %n) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %loop unwind label %u
loop:
  %i = phi i32 [ 0, %entry ], [ %i1, %body ]
  %c = icmp slt i32 %i, %n
  br i1 %c, label %body, label %exit
body:
  call void @sink(ptr addrspace(1) %o)
  %i1 = add i32 %i, 1
  br label %loop
exit:
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; Body's @sink escapes the obj inside the loop. After loop-simplify
; canonicalises the preheader, PEA's per-instruction escape materialise
; fires at the @sink call (NOT a preheader force-materialize). The alloc
; should still be present in IR.
; CHECK-LABEL: define void @test_simple_loop
; CHECK: invoke {{.*}}@jeandle.new_instance
; CHECK: call void @sink

!java-method-compilation = !{}
