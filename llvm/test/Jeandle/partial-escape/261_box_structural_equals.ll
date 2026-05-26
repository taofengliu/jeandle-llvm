; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" -jeandle-vm-callback-log=%S/Inputs/261_box_structural_equals.cblog %s | FileCheck %s

; Two boxed virtuals of klass 9999 (Integer, JBasicType::Int)
; with the SAME constant primitive value stored. `icmp eq` between the
; two boxed pointers must structural-fold to `true` (NOT to the default
; identity-based `false` for distinct virtuals). The effect is
; `IntegerEquals(boxA.value, boxB.value)` — and here both values are
; the same constant, so the result is the literal 1.
;
; The icmp folds and both allocations + stores are unused → fully eliminated.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)

declare i32 @__gxx_personality_v0(...)

define i1 @test_box_struct_eq()
    gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %a = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 9999 to ptr), i32 16)
       to label %na unwind label %u
na:
  %sa = getelementptr inbounds i8, ptr addrspace(1) %a, i64 12
  store atomic i32 42, ptr addrspace(1) %sa unordered, align 4
  %b = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 9999 to ptr), i32 16)
       to label %nb unwind label %u
nb:
  %sb = getelementptr inbounds i8, ptr addrspace(1) %b, i64 12
  store atomic i32 42, ptr addrspace(1) %sb unordered, align 4
  %eq = icmp eq ptr addrspace(1) %a, %b
  ret i1 %eq
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define i1 @test_box_struct_eq
; CHECK-NOT: jeandle.new_instance
; CHECK-NOT: store atomic
; CHECK-NOT: icmp
; CHECK: ret i1 true

!java-method-compilation = !{}
