; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Windows-EH funclet materialization under the reuse-OrigAlloc model. The
; object %o is allocated in `entry` (OUTSIDE any funclet) but only escapes
; (via @sink) INSIDE the cleanup funclet.
;
; Historically PEA virtualized %o in entry and emitted a fresh materialization
; invoke INSIDE the funclet, which had to carry a `[ "funclet"(token %pad) ]`
; operand bundle naming the enclosing funclet pad (synthesized via
; colorEHFunclets) or the verifier rejected it.
;
; Under reuse-OrigAlloc there is no materialization invoke: the original
; allocation %o (allocated outside the funclet, so it correctly carries no
; funclet bundle) is KEPT and dominates the escape. The tracked field store
; is replayed onto %o inside the funclet (a gep + store atomic onto %o), and
; @sink receives %o directly. The funclet bundle lives on the @sink call
; (where the IR author placed it), not on any new invoke.
;
; Jeandle does not currently target Windows; this is the IR-defensiveness
; rule (review requirement #6 -- PEA must tolerate any legal IR).

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @may_throw()
declare void @sink(ptr addrspace(1))
declare i32 @__CxxFrameHandler3(...)

define void @test_funclet_bundle() personality ptr @__CxxFrameHandler3 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 7 to ptr), i32 16)
       to label %afteralloc unwind label %ehalloc

afteralloc:
  %slot = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  store atomic i32 42, ptr addrspace(1) %slot unordered, align 4
  invoke void @may_throw()
      to label %cont unwind label %cleanup

cont:
  ret void

cleanup:
  %cp = cleanuppad within none []
  call void @sink(ptr addrspace(1) %o) [ "funclet"(token %cp) ]
  cleanupret from %cp unwind to caller

ehalloc:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @test_funclet_bundle
; The original allocation invoke is retained (allocated outside the funclet,
; so it carries no funclet bundle).
; CHECK: = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 7 to ptr), i32 16)
; Inside the cleanup funclet, the field store is replayed onto OrigAlloc %o.
; CHECK: %[[CP:[A-Za-z0-9._]+]] = cleanuppad within none []
; CHECK: %{{.*}} = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
; CHECK: store atomic i32 42, ptr addrspace(1) %{{.*}} unordered, align 4
; The escape consumes OrigAlloc %o directly; the funclet bundle stays on the
; @sink call (where the IR author placed it). No materialization invoke.
; CHECK: call void @sink(ptr addrspace(1) %o) [ "funclet"(token %[[CP]]) ]
; CHECK-NOT: pea.mat = invoke

!java-method-compilation = !{}
