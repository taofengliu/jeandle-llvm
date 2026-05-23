; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" -jeandle-vm-callback-log=%S/Inputs/262_box_structural_not_equals.cblog %s | FileCheck %s

; B10 Phase 3 (runtime path): two boxed virtuals of klass 9999 (Integer)
; storing non-constant primitive values. The structural fold replaces
; the pointer `icmp eq %a, %b` with a runtime `icmp eq i32 %x, %y` over
; the stored boxed values — distinguishable from the default identity
; fold (which would have produced the constant `i1 false`).
;
; The output therefore contains `icmp eq i32 %x, %y`, not `ret i1 false`.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)

declare i32 @__gxx_personality_v0(...)

define i1 @test_box_struct_runtime(i32 %x, i32 %y)
    gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %a = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 9999 to ptr), i32 16)
       to label %na unwind label %u
na:
  %sa = getelementptr inbounds i8, ptr addrspace(1) %a, i64 12
  store atomic i32 %x, ptr addrspace(1) %sa unordered, align 4
  %b = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 9999 to ptr), i32 16)
       to label %nb unwind label %u
nb:
  %sb = getelementptr inbounds i8, ptr addrspace(1) %b, i64 12
  store atomic i32 %y, ptr addrspace(1) %sb unordered, align 4
  %eq = icmp eq ptr addrspace(1) %a, %b
  ret i1 %eq
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; Phase 3 must rewrite the pointer compare into a runtime primitive
; compare. Identity-only would have returned `i1 false` here, since the
; two virtuals have distinct IDs.
; CHECK-LABEL: define i1 @test_box_struct_runtime
; CHECK-NOT: jeandle.new_instance
; CHECK-NOT: store atomic
; CHECK-NOT: icmp eq ptr addrspace(1)
; CHECK: icmp eq i32 %x, %y
; CHECK: ret i1

!java-method-compilation = !{}
