; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; C6 — classic irreducible CFG. Two blocks %a and %b each branch into
; the other, so the cycle has no natural-loop header (LoopInfo refuses
; to model it as a Loop at all). processLoop is never invoked for this
; region; the outer RPO walk processes each block once. The escape
; sites inside the cycle materialize the alloc per-escape, the alloc
; survives in IR, and there is no crash.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_irreducible_two_headers(i1 %p) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %dispatch unwind label %u
dispatch:
  br i1 %p, label %a, label %b
a:
  call void @sink(ptr addrspace(1) %o)
  br i1 %p, label %b, label %exit
b:
  call void @sink(ptr addrspace(1) %o)
  br i1 %p, label %a, label %exit
exit:
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; No crash, alloc survives (potentially materialised at the first
; escape inside the cycle), both sink calls intact.
; CHECK-LABEL: define void @test_irreducible_two_headers
; CHECK: invoke {{.*}}@jeandle.new_instance
; CHECK-DAG: call void @sink
; CHECK-DAG: call void @sink

!java-method-compilation = !{}
