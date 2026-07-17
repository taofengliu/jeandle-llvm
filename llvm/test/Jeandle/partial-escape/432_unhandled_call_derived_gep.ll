; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Generic dispatch fallback (processInstruction) under the reuse-OrigAlloc
; model: an UNHANDLED call consuming a derived-GEP operand of a virtual object.
;
; t_derived_off16 / t_derived_off0: materialising at the call would place a
; fresh allocation after the GEP, so the GEP's base would not be dominated.
; materializeVirtualOperandsSafely detects the derived operand structurally
; (V != AllocationCall -- NOT by offset; offset-0 derived GEPs poison just
; like non-zero ones) and keeps the object real (markIneligible) so the GEP
; stays valid. The IR is unchanged: %o retained, the GEP keeps its real base
; %o, and the call keeps the derived %g.
;
; t_whole_object: a whole-object call argument is OrigAlloc directly. Under
; reuse-OrigAlloc the object escapes via @external(%o): the original %o is
; retained and consumed directly (no fresh materialization invoke is needed,
; so no %pea.mat).

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @external(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @t_derived_off16() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 32)
         to label %n unwind label %u
n:
  %g = getelementptr inbounds i8, ptr addrspace(1) %o, i64 16
  store atomic i32 42, ptr addrspace(1) %g unordered, align 4
  call void @external(ptr addrspace(1) %g)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

define void @t_derived_off0() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 32)
         to label %n unwind label %u
n:
  %g = getelementptr inbounds i8, ptr addrspace(1) %o, i64 0
  call void @external(ptr addrspace(1) %g)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

define void @t_whole_object() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 32)
         to label %n unwind label %u
n:
  call void @external(ptr addrspace(1) %o)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; Derived-operand calls: the object stays real (markIneligible) and the GEP
; keeps a real base. CHECK-NOT poison right after each LABEL so its scope
; spans the body where a pre-fix poison GEP would sit.
; CHECK-LABEL: define void @t_derived_off16
; CHECK-NOT: getelementptr{{.*}}poison
; CHECK: invoke{{.*}}@jeandle.new_instance
; CHECK: call void @external(ptr addrspace(1) %g)

; CHECK-LABEL: define void @t_derived_off0
; CHECK-NOT: getelementptr{{.*}}poison
; CHECK: invoke{{.*}}@jeandle.new_instance
; CHECK: call void @external(ptr addrspace(1) %g)

; Whole-object call: OrigAlloc %o retained and consumed directly (no fresh
; materialization invoke; reuse-OrigAlloc needs none).
; CHECK-LABEL: define void @t_whole_object
; CHECK: invoke{{.*}}@jeandle.new_instance
; CHECK: call void @external(ptr addrspace(1) %o)
; CHECK-NOT: pea.mat = invoke

!java-method-compilation = !{}
