; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Dangling field snapshot, deopt descriptor path (review §3 #2). Scalar load
; from a virtual VO stored into another virtual VO (o.f = p.g), then the
; outer VO is described in a deopt bundle. The folded load (%lg) gets a
; scalar alias + a ReplaceLoad effect that erases it in Pass 1;
; RewriteDeoptBundleEffect::apply must emit the NORMALIZED terminal %x in
; the VO descriptor, not the freed %lg.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(i32)
declare i32 @__gxx_personality_v0(...)

define void @scalar_folded_load_in_descriptor(i32 %x) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %p = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 100 to ptr), i32 16)
       to label %n1 unwind label %u
n1:
  %pf = getelementptr inbounds i8, ptr addrspace(1) %p, i64 8
  store atomic i32 %x, ptr addrspace(1) %pf unordered, align 4
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 200 to ptr), i32 16)
       to label %n2 unwind label %u
n2:
  %of = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  %lg = load atomic i32, ptr addrspace(1) %pf unordered, align 4
  ; o.f = p.g  (p.g folds to %x; %lg gets a scalar alias + ReplaceLoad effect)
  store atomic i32 %lg, ptr addrspace(1) %of unordered, align 4
  ; o lives only in the deopt bundle (NeverEscapes, describable).
  call void @sink(i32 %x)
       [ "deopt"(i32 99, i32 99, i64 12, ptr addrspace(1) %o) ]
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; Both VOs are NeverEscapes (eliminated); %o is described in the bundle.
; vo-ids: %p=0 (allocated first, described nowhere — it has no bundle use and
; is not referenced by %o's descriptor... wait: %o's only field is the scalar
; %x, so %p needs no descriptor; %o gets vo-id 1 after %p's registration).
; Descriptor %o (vo_id=1, ScalarValueType, T_OBJECT): (1<<32)|(4<<16)|12 = 4295229452
; CHECK-LABEL: define void @scalar_folded_load_in_descriptor(
; CHECK-NOT: jeandle.new_instance
; CHECK: call void @sink(i32 %x)
; CHECK-SAME: [ "deopt"(i32 99, i32 99,
; CHECK-SAME: i64 4295229452, i64 200, i32 1,
; field (offset 8, LocalType/T_INT): (8<<32)|10 = 34359738378 -> value %x (the
; normalized terminal, NOT the erased %lg)
; CHECK-SAME: i64 34359738378, i32 %x,
; the OrigAlloc locals slot becomes a VORefLocalType reference (vo_id=1):
; (1<<32)|(8<<16)|12 = 4295491596, followed by i32 1.
; CHECK-SAME: i64 4295491596, i32 1) ]
; CHECK-NOT: poison

!java-method-compilation = !{}
