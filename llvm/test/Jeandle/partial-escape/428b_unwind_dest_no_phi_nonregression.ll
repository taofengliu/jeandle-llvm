; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Non-regression for #2.1: when the original alloc's unwind dest has NO phi
; nodes (the common OOM-cleanup shape: landingpad + resume), reuse is safe and
; still preferred — no pea.unwind block is synthesized.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_unwind_dest_no_phi() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
         to label %n unwind label %uw
n:
  call void @sink(ptr addrspace(1) %o)
  ret void
uw:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @test_unwind_dest_no_phi
; CHECK: invoke hotspotcc{{.*}}@jeandle.new_instance
; The materialization invoke reuses the PHI-less %uw; no synthesized block.
; CHECK-NOT: pea.unwind
; CHECK: call void @sink(ptr addrspace(1)

!java-method-compilation = !{}
