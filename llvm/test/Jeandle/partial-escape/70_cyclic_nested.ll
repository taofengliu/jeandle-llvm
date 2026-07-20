; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Cyclic nested virtuals. Two virtuals A and B form a cycle —
; A.f = B and B.g = A. Returning A escapes A; B escapes transitively (A.f
; references it). Under reuse-OrigAlloc both OrigAllocs are KEPT (each
; dominates the single escape point), so every field store replays directly
; onto its OrigAlloc and the cycle resolves through the peer's OrigAlloc — no
; cascade coordination, no fresh pea.mat invoke, no materialized-object PHI.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare i32 @__gxx_personality_v0(...)

define ptr addrspace(1) @test_cyclic_nested() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %a = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 11111 to ptr), i32 16)
       to label %nA unwind label %u1
nA:
  %b = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 22222 to ptr), i32 16)
       to label %nB unwind label %u2
nB:
  %slotA = getelementptr inbounds i8, ptr addrspace(1) %a, i64 8
  store atomic ptr addrspace(1) %b, ptr addrspace(1) %slotA unordered, align 8
  %slotB = getelementptr inbounds i8, ptr addrspace(1) %b, i64 8
  store atomic ptr addrspace(1) %a, ptr addrspace(1) %slotB unordered, align 8
  ret ptr addrspace(1) %a
u1:
  %lp1 = landingpad i64 cleanup
  resume i64 %lp1
u2:
  %lp2 = landingpad i64 cleanup
  resume i64 %lp2
}

; Both OrigAllocs (klass 11111 = A, klass 22222 = B) are retained.
; CHECK-LABEL: define ptr addrspace(1) @test_cyclic_nested
; CHECK-DAG: invoke hotspotcc{{.*}}ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 11111 to ptr), i32 16)
; CHECK-DAG: invoke hotspotcc{{.*}}ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 22222 to ptr), i32 16)
; No fresh materialization invoke is emitted.
; CHECK-NOT: pea.mat = invoke
; Both replayed field stores use OrigAlloc values — the back edge B.g = A
; resolves through A's OrigAlloc (kept alive), never poison.
; CHECK: store atomic ptr addrspace(1) %a, ptr addrspace(1) %pea.matslot{{[0-9]*}} unordered, align 8
; CHECK: store atomic ptr addrspace(1) %b, ptr addrspace(1) %pea.matslot{{[0-9]*}} unordered, align 8
; CHECK-NOT: poison
; CHECK: ret ptr addrspace(1) %a

!java-method-compilation = !{}
