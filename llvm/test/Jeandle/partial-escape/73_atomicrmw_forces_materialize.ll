; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; B17: atomicrmw on a virtual field at a constant offset is compile-time-
; folded. The slot's prior value is what the atomicrmw "returns" (RAUW'd
; replacement); the new value is recorded as the field's tracked entry.
; No other thread can race a virtual, so the atomic + ordering semantics
; are trivially satisfied — no fence needed.
;
; Here: store 0; atomicrmw add 5. The OLD value is 0 (the prior store), so
; the atomicrmw replaces with 0 and the return is `ret i32 0`. The
; allocation, store, and atomicrmw are all eliminated.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare i32 @__gxx_personality_v0(...)

define i32 @test_atomicrmw() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %n unwind label %u
n:
  %s = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  store atomic i32 0, ptr addrspace(1) %s unordered, align 4
  %old = atomicrmw add ptr addrspace(1) %s, i32 5 seq_cst, align 4
  ret i32 %old
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define i32 @test_atomicrmw
; CHECK-NOT: jeandle.new_instance
; CHECK-NOT: atomicrmw
; CHECK-NOT: store
; CHECK: ret i32 0

!java-method-compilation = !{}
