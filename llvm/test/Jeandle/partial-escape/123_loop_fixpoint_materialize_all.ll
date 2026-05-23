; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; A1 — MATERIALIZE_ALL fallback exercise. The body contains a real
; escape (call to @sink with the pointer) on every iteration so the
; object MUST materialize. Without A1 the preheader-force-materialize
; safety net handles this; with A1 the fixpoint converges with the
; object marked Materialized at the loop body, and the per-instruction
; materializeAt emits the materialization invoke at the alloc site.
; Either way the alloc must survive in IR.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_mat_all_in_body(i32 %n) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %prep unwind label %u
prep:
  br label %loop
loop:
  %i = phi i32 [ 0, %prep ], [ %i1, %body ]
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

; The materialization invoke must appear before @sink is reachable, and
; @sink must still see a real pointer. The original alloc invoke is
; eliminated (its slot is filled by the materialization invoke instead).
; CHECK-LABEL: define void @test_mat_all_in_body
; CHECK: invoke {{.*}}@jeandle.new_instance
; CHECK: call void @sink

!java-method-compilation = !{}
