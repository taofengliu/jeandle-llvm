; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s
; XFAIL: *

; Mutual reference (o.f = p, p.g = o). EXPECTED TO FAIL — see
; TODO(cyclic-field-materialize) in PartialEscapeTransform.cpp. A back-edge field
; (p.g = o, where o is materialized later in the same cascade) has no dominating
; NewInv: each materialize invoke is a block terminator, so a cascade's NewInvs
; chain across blocks and the later NewInv cannot dominate the earlier field
; store. resolveMaterializedUses leaves the OrigAlloc use and Pass 2 turns it to
; poison. The forward edge (o.f = p) resolves correctly; only the back edge
; miscompiles. Graal commits every object at a point in one CommitAllocationNode;
; Jeandle needs a two-phase transform (create every cascade NewInv, then emit
; field stores) to match. The CHECKs below assert the correct post-fix behavior,
; so this flips to XPASS once the two-phase transform lands.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @mutual_field_perpred(i1 %c)
    gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 11111 to ptr), i32 16) to label %oi unwind label %u
oi:
  %p = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 22222 to ptr), i32 16) to label %fld unwind label %u
fld:
  %of = getelementptr inbounds i8, ptr addrspace(1) %o, i64 0
  store atomic ptr addrspace(1) %p, ptr addrspace(1) %of unordered, align 8
  %pg = getelementptr inbounds i8, ptr addrspace(1) %p, i64 8
  store atomic ptr addrspace(1) %o, ptr addrspace(1) %pg unordered, align 8
  br i1 %c, label %left, label %right
left:
  br label %merge
right:
  br label %merge
merge:
  call void @sink(ptr addrspace(1) %o)
  call void @sink(ptr addrspace(1) %p)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

!java-method-compilation = !{}

; CHECK-LABEL: define void @mutual_field_perpred
; Each object is per-pred materialized at both preds (4 NewInvs total).
; CHECK: invoke hotspotcc{{.*}}@jeandle.new_instance(ptr inttoptr (i64 11111 to ptr)
; CHECK: invoke hotspotcc{{.*}}@jeandle.new_instance(ptr inttoptr (i64 11111 to ptr)
; CHECK: invoke hotspotcc{{.*}}@jeandle.new_instance(ptr inttoptr (i64 22222 to ptr)
; CHECK: invoke hotspotcc{{.*}}@jeandle.new_instance(ptr inttoptr (i64 22222 to ptr)
; Replayed field stores use real per-pred NewInvs (no <badref>).
; CHECK: store atomic ptr addrspace(1) %pea.mat{{[0-9]*}}, ptr addrspace(1) %pea.matslot unordered, align 8
; CHECK: store atomic ptr addrspace(1) %pea.mat{{[0-9]*}}, ptr addrspace(1) %pea.matslot unordered, align 8
; CHECK: ret void
