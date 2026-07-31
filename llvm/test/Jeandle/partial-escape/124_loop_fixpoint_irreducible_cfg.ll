; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Irreducible CFG (two entry points to a cycle). LoopInfo refuses to
; model it as a natural loop; processLoop's "no preheader" branch falls
; through to the pessimistic single-pass walk, and the per-instruction
; escape handler covers any in-cycle escape. The alloc in entry survives
; via materialise-at-escape inside the cycle.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_irreducible(i1 %p) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %dispatch unwind label %u
dispatch:
  br i1 %p, label %a, label %b
a:
  call void @sink(ptr addrspace(1) %o)
  br label %b
b:
  call void @sink(ptr addrspace(1) %o)
  br i1 %p, label %a, label %exit
exit:
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; The alloc must survive (escaped to @sink). Both @sink calls must be
; intact.
; CHECK-LABEL: define void @test_irreducible
; CHECK: invoke {{.*}}@jeandle.new_instance
; CHECK: call void @sink
; CHECK: call void @sink

!java-method-compilation = !{}
