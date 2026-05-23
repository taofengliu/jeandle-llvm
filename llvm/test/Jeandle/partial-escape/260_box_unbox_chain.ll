; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" -jeandle-vm-callback-log=%S/Inputs/260_box_unbox_chain.cblog %s | FileCheck %s

; B10: synthetic autobox + unbox chain. The IR shape mirrors what the
; future JDK frontend lowering of `Integer.intValue(Integer.valueOf(x))`
; would emit: a `jeandle.new_instance(Integer)` followed by a primitive
; store into the value slot, then a load from the same slot. PEA must
; eliminate the allocation and the store, and fold the load to the
; stored primitive value (the standard alloc+store+load fold from B2 —
; this test exists to confirm the B10 box tagging in tier1Allocate does
; not perturb the basic chain).
;
; Klass 9999 is registered as Integer (JBasicType::Int=4) via the cblog
; so VirtualObject::BoxedPrimitiveKind is set; with no escape downstream
; the box-specific fold paths (Phase 3 / Phase 4) are not exercised, but
; the IsBoxed VMCallback invocation IS exercised.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)

declare i32 @__gxx_personality_v0(...)

define i32 @test_box_unbox_chain(i32 %x)
    gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 9999 to ptr), i32 16)
       to label %n unwind label %u
n:
  %slot = getelementptr inbounds i8, ptr addrspace(1) %o, i64 12
  store atomic i32 %x, ptr addrspace(1) %slot unordered, align 4
  %v = load atomic i32, ptr addrspace(1) %slot unordered, align 4
  ret i32 %v
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define i32 @test_box_unbox_chain
; CHECK-NOT: jeandle.new_instance
; CHECK-NOT: store atomic
; CHECK-NOT: load atomic
; CHECK: ret i32 %x

!java-method-compilation = !{}
