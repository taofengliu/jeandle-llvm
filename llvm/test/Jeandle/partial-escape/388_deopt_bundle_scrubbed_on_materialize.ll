; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; A PartiallyEscapes VO that is STILL VIRTUAL at a safepoint is described
; by a VO descriptor in the deopt bundle, even though it escapes later. Here
; %o has two scalar fields and escapes via the @sink call argument, so it is
; PartiallyEscapes (OrigAlloc is KEPT and its field stores replayed before the
; escape). But at the sink's safepoint — which the analyzer visits BEFORE
; materializing the call-argument operand (recordDeoptBundleMappings runs ahead
; of materializeVirtualOperandsSafely) — %o is still virtual, so the deopt
; bundle's %o slot is rewritten to a VORef + a ScalarValueType descriptor
; (klass + the two scalar field values). At deopt here the object hasn't
; escaped yet, so HotSpot reallocating it from the descriptor is sound (no
; external reference exists). This is the Graal/C2 analogue: a scalar-replaced
; object is described as virtual at every pre-materialization safepoint.
;
; (The sibling 641_escape_via_call_arg_no_vo_descriptor.ll covers the contrasting
; case where the VO has REFERENCE fields, so the ref-field bail leaves it
; undescribed.)

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @partial_escape_descriptor_at_safepoint(i32 %a, i32 %b) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 24)
       to label %n unwind label %u
n:
  %s1 = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  %s2 = getelementptr inbounds i8, ptr addrspace(1) %o, i64 16
  store atomic i32 %a, ptr addrspace(1) %s1 unordered, align 4
  store atomic i32 %b, ptr addrspace(1) %s2 unordered, align 4
  ; %o escapes via the @sink argument AND is in the deopt bundle (a locals
  ; slot, enc(LocalType, index=0, T_OBJECT)=12). At this safepoint %o is still
  ; virtual → described by a VO descriptor; OrigAlloc is kept for the escape.
  call void @sink(ptr addrspace(1) %o)
       [ "deopt"(i32 99, i32 99, i64 12, ptr addrspace(1) %o) ]
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @partial_escape_descriptor_at_safepoint
; OrigAlloc is RETAINED (PartiallyEscapes — it escapes via @sink). No pea.mat.
; CHECK: invoke hotspotcc ptr addrspace(1) @jeandle.new_instance
; CHECK-NOT: pea.mat = invoke
; Tracked field stores are replayed onto OrigAlloc before the escape.
; CHECK: store atomic i32 %a, ptr addrspace(1) %pea.matslot unordered, align 4
; CHECK: store atomic i32 %b, ptr addrspace(1) %pea.matslot{{[0-9]*}} unordered, align 4
; The sink still receives OrigAlloc directly (the real, materialized object).
; CHECK: call void @sink(ptr addrspace(1) %o)
; CHECK-SAME: [ "deopt"(
; duplicated-BCI marker preserved.
; CHECK-SAME: i32 99, i32 99,
; ScalarValueType VO descriptor header (vo_id=0): (0<<32)|(4<<16)|12 = 262156
; CHECK-SAME: i64 262156, i64 12345, i32 2,
; field 0 (offset 8, LocalType/T_INT): (8<<32)|10 = 34359738378 -> value %a
; CHECK-SAME: i64 34359738378, i32 %a,
; field 1 (offset 16, LocalType/T_INT): (16<<32)|10 = 68719476746 -> value %b
; CHECK-SAME: i64 68719476746, i32 %b,
; the OrigAlloc locals slot is replaced by a VORefLocalType reference (vo_id=0):
; (0<<32)|(8<<16)|12 = 524300, followed by vo_id i32 0.
; CHECK-SAME: i64 524300, i32 0) ]

!java-method-compilation = !{}
