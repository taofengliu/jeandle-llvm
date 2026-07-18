; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Commit availability sweep (review §3 #2): o.f = p.g where p.g is a MERGE
; field-PHI (analyzer-built, owned by p's CreatePHI effect). p then becomes
; ineligible (derived-pointer escape) -> dropEffectsFor(p) drops the
; CreatePHI AND the ReplaceLoad that folded %lg -> %lg survives as a real
; load. o's surviving Materialize would replay the never-to-be-inserted PHI
; -> the sweep must keep o real too (its whole effect set is dropped), so
; the o.f store survives as a real store of the real load.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare void @sinkp(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @phi_producer_dropped(i1 %c) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %p = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 100 to ptr), i32 16)
       to label %np unwind label %u
np:
  %pg = getelementptr inbounds i8, ptr addrspace(1) %p, i64 8
  br i1 %c, label %t, label %f
t:
  store atomic i32 1, ptr addrspace(1) %pg unordered, align 4
  br label %m
f:
  store atomic i32 2, ptr addrspace(1) %pg unordered, align 4
  br label %m
m:
  ; p.g is a merge field-PHI here; %lg folds to it.
  %lg = load atomic i32, ptr addrspace(1) %pg unordered, align 4
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 200 to ptr), i32 16)
       to label %no unwind label %u
no:
  %of = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  store atomic i32 %lg, ptr addrspace(1) %of unordered, align 4
  ; derived-pointer escape of p -> p ineligible -> CreatePHI dropped.
  %gp = getelementptr inbounds i8, ptr addrspace(1) %p, i64 4
  call void @sinkp(ptr addrspace(1) %gp)
  ; o escapes: without the sweep its Materialize would replay the orphan PHI.
  call void @sink(ptr addrspace(1) %o)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; Everything stays real: both allocations retained, %lg is a real load, the
; o.f store is a real store, no orphan PHI, no poison.
; CHECK-LABEL: define void @phi_producer_dropped(
; CHECK: invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 100 to ptr), i32 16)
; CHECK: %lg = load atomic i32, ptr addrspace(1) %pg unordered, align 4
; CHECK: invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 200 to ptr), i32 16)
; CHECK: store atomic i32 %lg, ptr addrspace(1) %of unordered, align 4
; CHECK: call void @sink(ptr addrspace(1) %o)
; CHECK-NOT: pea.field.phi
; CHECK-NOT: poison

!java-method-compilation = !{}
