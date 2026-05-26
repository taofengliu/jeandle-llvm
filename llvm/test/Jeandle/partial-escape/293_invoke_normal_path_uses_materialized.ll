; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Exception edge state splitting — regression test for the NORMAL
; successor of an invoke that materialized one of its operands. A
; separate pre-invoke snapshot is used for the unwind successor, but
; the normal successor must continue to inherit the post-invoke state
; (i.e. it sees VO_A as Materialized, with the materialized invoke as
; its backing pointer).
;
; A subsequent use of VO_A on the normal path must thread through the
; materialized invoke. The test below uses @sink2 (a second opaque
; consumer on the normal path) to anchor the materialized value; the
; CHECK requires it to take a real pointer rather than the original
; (eliminated) %a.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare void @sink2(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_293() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %a = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 11111 to ptr), i32 16)
       to label %n unwind label %u_a
n:
  ; First @sink escapes VO_A, materializing it before the invoke.
  invoke void @sink(ptr addrspace(1) %a)
       to label %nfinal unwind label %handler
nfinal:
  ; Normal successor: another consumer of VO_A. Must see the materialized
  ; pointer (the freshly-emitted new_instance invoke), not the original
  ; (which gets eliminated).
  call void @sink2(ptr addrspace(1) %a)
  ret void
handler:
  %lp = landingpad i64 cleanup
  resume i64 %lp
u_a:
  %lpa = landingpad i64 cleanup
  resume i64 %lpa
}

; The original allocation invoke is eliminated; a single new_instance
; invoke survives as the materialization point before @sink. Both @sink
; and @sink2 must thread through the SAME materialized pointer.
; CHECK-LABEL: define void @test_293
; CHECK: %[[MAT:[A-Za-z0-9._]+]] = invoke hotspotcc{{.*}}ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 11111 to ptr), i32 16)
; CHECK: invoke void @sink(ptr addrspace(1) %[[MAT]])
; CHECK: call void @sink2(ptr addrspace(1) %[[MAT]])

!java-method-compilation = !{}
