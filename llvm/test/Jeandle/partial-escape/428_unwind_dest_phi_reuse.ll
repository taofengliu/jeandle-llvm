; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; findOrSynthesizeUnwindDest PHI reuse under the reuse-OrigAlloc model. %uw
; has TWO invoke predecessors (the alloc %o and a second may_throw invoke)
; and a USED PHI (`%sel`) over them.
;
; Historically, when %o escaped and was materialized, the fresh materialization
; invoke needed an unwind dest; Strategy 1 reused %uw verbatim without
; checking phis().empty(), making its block a predecessor of %uw with no
; matching PHI incoming -- a PHI/predecessor mismatch the verifier rejects.
; After the fix, a PHI-carrying unwind dest was not reused; a synthesized
; pea.unwind block was.
;
; Under reuse-OrigAlloc there is NO fresh materialization invoke -- the
; original allocation %o is retained with its ORIGINAL unwind dest %uw -- so
; no unwind-dest choice is made at all. %uw keeps exactly its two original
; predecessors (entry, cont) and its `%sel` PHI is intact; no pea.unwind block
; is synthesized. The escape consumes %o directly.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @may_throw()
declare void @sink(ptr addrspace(1))
declare void @use_i32(i32)
declare i32 @__gxx_personality_v0(...)

define void @test_unwind_dest_phi_reuse() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
  ptr inttoptr (i64 12345 to ptr), i32 16)
  to label %cont unwind label %uw
cont:
  invoke void @may_throw() to label %escape unwind label %uw
escape:
  call void @sink(ptr addrspace(1) %o)
  ret void
uw:
  %sel = phi i32 [ 0, %entry ], [ 1, %cont ]
  %lp = landingpad i64 cleanup
  call void @use_i32(i32 %sel)
  resume i64 %lp
}

; CHECK-LABEL: define void @test_unwind_dest_phi_reuse
; The original allocation invoke is retained with its original unwind dest
; %uw (no fresh materialize invoke, so no unwind-dest choice is made).
; CHECK: invoke hotspotcc{{.*}}@jeandle.new_instance
; CHECK: to label %cont unwind label %uw
; The escape consumes OrigAlloc %o directly (no materialize invoke inserted).
; CHECK: call void @sink(ptr addrspace(1) %o)
; %uw's PHI is intact with exactly its two original predecessors (entry, cont)
; -- no synthesized pea.unwind block was needed and the PHI was not corrupted
; by a path-wrong incoming.
; CHECK: %sel = phi i32 [ 0, %entry ], [ 1, %cont ]
; CHECK-NOT: pea.unwind
; CHECK-NOT: pea.mat = invoke

!java-method-compilation = !{}
