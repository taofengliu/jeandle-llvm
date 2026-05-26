; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Cmpxchg on a virtual field at a constant offset is compile-time-
; folded when Expected and the slot's current entry are both constants.
; The equality is evaluated and either updates the field entry (success) or
; leaves it unchanged (failure); the result struct {prior, equal?} is
; synthesized as a Constant and RAUW'd to the cmpxchg.
;
; Here: store 0; cmpxchg expected=0 → equal. Result is {0, true}; the
; subsequent extractvalue returns 0.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare i32 @__gxx_personality_v0(...)

define i32 @test_cmpxchg() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %n unwind label %u
n:
  %s = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  store atomic i32 0, ptr addrspace(1) %s unordered, align 4
  %p = cmpxchg ptr addrspace(1) %s, i32 0, i32 1 seq_cst seq_cst, align 4
  %v = extractvalue { i32, i1 } %p, 0
  ret i32 %v
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define i32 @test_cmpxchg
; CHECK-NOT: jeandle.new_instance
; CHECK-NOT: cmpxchg
; CHECK-NOT: store atomic
; CHECK: extractvalue { i32, i1 } { i32 0, i1 true }, 0
; CHECK: ret i32

!java-method-compilation = !{}
