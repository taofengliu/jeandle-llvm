; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Exception edge state splitting — regression test for a handler
; whose only purpose is to re-throw via `resume`. Such handlers have no
; per-object state of their own; the state-split must not leave the
; analyzer in a confused state (e.g. by treating the pre-invoke snapshot
; as authoritative for a sink-only handler).
;
; In particular, the materialization Effect emitted by the invoke is
; inserted BEFORE the invoke, so the materialized invoke is also a
; terminator of the entry block; the handler block must still be
; well-formed after the transform (landingpad + resume present, no
; dangling SSA references).

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_292() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %a = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 11111 to ptr), i32 16)
       to label %n unwind label %u_a
n:
  ; This invoke materializes VO_A. The unwind successor is a
  ; re-throwing handler that ignores VO_A entirely.
  invoke void @sink(ptr addrspace(1) %a)
       to label %nfinal unwind label %handler
nfinal:
  ret void
handler:
  %lp = landingpad i64 cleanup
  resume i64 %lp
u_a:
  %lpa = landingpad i64 cleanup
  resume i64 %lpa
}

; VO_A materializes for the @sink call. The materialization invoke must
; reuse VO_A's klass. The handler must still terminate with `resume`.
; CHECK-LABEL: define void @test_292
; CHECK: invoke hotspotcc{{.*}}ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 11111 to ptr), i32 16)
; CHECK: invoke void @sink
; CHECK: resume i64

!java-method-compilation = !{}
