; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Mixed forward and back edges among three objects: A.f = B and B.g = C are
; forward, while C.h = A is a back edge. Returning A escapes the whole group.
; Under reuse-OrigAlloc all three OrigAllocs are KEPT (each dominates the single
; escape point), so every field store replays directly onto its OrigAlloc and
; the back edge C.h = A resolves through A's OrigAlloc — no cascade
; coordination, no fresh pea.mat invoke, no materialized-object PHI.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare i32 @__gxx_personality_v0(...)

define ptr addrspace(1) @cyclic_and_forward_chain_mixed()
    gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %a = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 11111 to ptr), i32 24) to label %na unwind label %u1
na:
  %b = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 22222 to ptr), i32 24) to label %nb unwind label %u2
nb:
  %c = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 33333 to ptr), i32 24) to label %nc unwind label %u3
nc:
  %sa = getelementptr inbounds i8, ptr addrspace(1) %a, i64 8
  store atomic ptr addrspace(1) %b, ptr addrspace(1) %sa unordered, align 8
  %sb = getelementptr inbounds i8, ptr addrspace(1) %b, i64 8
  store atomic ptr addrspace(1) %c, ptr addrspace(1) %sb unordered, align 8
  %sc = getelementptr inbounds i8, ptr addrspace(1) %c, i64 16
  store atomic ptr addrspace(1) %a, ptr addrspace(1) %sc unordered, align 8
  ret ptr addrspace(1) %a
u1:
  %lp1 = landingpad i64 cleanup
  resume i64 %lp1
u2:
  %lp2 = landingpad i64 cleanup
  resume i64 %lp2
u3:
  %lp3 = landingpad i64 cleanup
  resume i64 %lp3
}

!java-method-compilation = !{}

; CHECK-LABEL: define ptr addrspace(1) @cyclic_and_forward_chain_mixed
; CHECK-DAG: invoke hotspotcc{{.*}}@jeandle.new_instance(ptr inttoptr (i64 11111 to ptr)
; CHECK-DAG: invoke hotspotcc{{.*}}@jeandle.new_instance(ptr inttoptr (i64 22222 to ptr)
; CHECK-DAG: invoke hotspotcc{{.*}}@jeandle.new_instance(ptr inttoptr (i64 33333 to ptr)
; No fresh materialization invoke is emitted.
; CHECK-NOT: pea.mat = invoke
; All three replayed field stores use OrigAlloc values — the back edge C.h = A
; resolves through A's OrigAlloc (kept alive), never poison.
; CHECK: store atomic ptr addrspace(1) %a, ptr addrspace(1) %pea.matslot{{[0-9]*}} unordered, align 8
; CHECK: store atomic ptr addrspace(1) %c, ptr addrspace(1) %pea.matslot{{[0-9]*}} unordered, align 8
; CHECK: store atomic ptr addrspace(1) %b, ptr addrspace(1) %pea.matslot{{[0-9]*}} unordered, align 8
; CHECK-NOT: poison
; CHECK: ret ptr addrspace(1) %a
