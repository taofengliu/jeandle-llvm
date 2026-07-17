; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Lock-mismatch on A across a diamond, with two objects (A and B) live, under
; the reuse-OrigAlloc model.
;
;   then: monitorenter(B) then monitorenter(A) — both tracked virtually; in
;         the old model the narrow strict-lock-order rule would have cascaded
;         B's materialization onto A's at the then-pred.
;   else: no locks.
;   merge: lock counts on A disagree (1 vs 0); lock counts on B disagree
;          (1 vs 0).
;
; Under reuse-OrigAlloc neither object is re-materialized per pred: the
; ORIGINAL allocations (OrigAlloc %oA and OrigAlloc %oB) dominate every use
; and are kept verbatim. Both surviving monitorenters stay in their original
; block, receivers pointing at their own OrigAlloc. No fresh materialization
; invoke is emitted for either object, no cascade fires, and no PHI is built.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc void @jeandle.monitorenter_with_thin_lock(ptr addrspace(1), ptr)
declare i32 @__gxx_personality_v0(...)

define void @test_lock_mismatch_with_cascade(i1 %c) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %lock = alloca i64, align 8
  %oA = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
              ptr inttoptr (i64 11111 to ptr), i32 16)
       to label %allocB unwind label %u
allocB:
  %oB = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
              ptr inttoptr (i64 22222 to ptr), i32 16)
       to label %branch unwind label %u
branch:
  br i1 %c, label %then, label %else
then:
  call hotspotcc void @jeandle.monitorenter_with_thin_lock(
              ptr addrspace(1) %oB, ptr %lock)
  call hotspotcc void @jeandle.monitorenter_with_thin_lock(
              ptr addrspace(1) %oA, ptr %lock)
  br label %merge
else:
  br label %merge
merge:
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @test_lock_mismatch_with_cascade
; Both ORIGINAL allocation invokes are RETAINED (no fresh materialization).
; CHECK: %oA = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 11111 to ptr), i32 16)
; CHECK: %oB = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 22222 to ptr), i32 16)
; No pea.mat materialization invoke is emitted.
; CHECK-NOT: pea.mat = invoke
; Both surviving monitorenters stay in their original block; each receiver is
; the correct object's OrigAlloc (B's enter on %oB, A's enter on %oA), in
; their original bytecode order.
; CHECK: call hotspotcc void @jeandle.monitorenter_with_thin_lock(ptr addrspace(1) %oB, ptr %lock)
; CHECK: call hotspotcc void @jeandle.monitorenter_with_thin_lock(ptr addrspace(1) %oA, ptr %lock)
; Exactly two monitorenters — no synthesized enters, no per-object duplication.
; CHECK-NOT: call hotspotcc void @jeandle.monitorenter_with_thin_lock

!java-method-compilation = !{}
