; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; PEA-Plan §B7 risk note: a self-referencing PHI in a loop header.
; processBlockPhis handles the loop PHI via its existing fixpoint
; logic; downstream consumers of the PHI may query resolveVirtualRef,
; which encounters the PHI's self-incoming and must terminate via the
; on-stack Visited check rather than infinite-recursing.
;
; The expected outcome: no analyzer crash. The alloc is preallocated
; before the loop and materialized by materializeBeforeLoops; the loop
; PHI sees the materialized pointer on the back-edge and behaves as a
; regular real-pointer PHI. The test pins "no crash" + the alloc
; survives as a real allocation.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @use(i32)
declare i32 @__gxx_personality_v0(...)

define void @test_cycle(i32 %n, i1 %c)
    gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %ph unwind label %u
ph:
  br label %loop
loop:
  ; Self-referencing PHI: incoming from preheader is %o, incoming from
  ; back-edge is the PHI itself.
  %phi = phi ptr addrspace(1) [ %o, %ph ], [ %phi, %loop ]
  ; A consumer that queries resolveVirtualRef on %phi: the icmp eq
  ; against %o. Without on-stack cycle detection in
  ; resolveVirtualRefImpl, the PHI->PHI back-edge would infinite-recurse.
  %eq = icmp eq ptr addrspace(1) %phi, %o
  br i1 %c, label %loop, label %exit
exit:
  br i1 %eq, label %same, label %diff
same:
  call void @use(i32 1)
  ret void
diff:
  call void @use(i32 -1)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; No crash; the alloc is real (materialized before the loop). Either
; arm of the icmp may or may not be folded — the key property is that
; the analyzer terminates and the function verifies.
; CHECK-LABEL: define void @test_cycle
; CHECK: @jeandle.new_instance

!java-method-compilation = !{}
