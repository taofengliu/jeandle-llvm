; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; An atomicrmw updates the field-state entry; a subsequent load on the
; same slot sees the new value (whether the slot is constant or carries an
; unparented binop). Covers both a constant case (add 3 to a constant 10
; → slot = 13) and a non-constant case (add %k to slot 13 → slot becomes
; `add 13, %k`).

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare i32 @__gxx_personality_v0(...)

define i32 @t_const() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %n unwind label %u
n:
  %s = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  store atomic i32 10, ptr addrspace(1) %s unordered, align 4
  %old = atomicrmw add ptr addrspace(1) %s, i32 3 seq_cst, align 4
  %v = load atomic i32, ptr addrspace(1) %s unordered, align 4
  ret i32 %v
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

define i32 @t_nonconst(i32 %k) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %n unwind label %u
n:
  %s = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  store atomic i32 13, ptr addrspace(1) %s unordered, align 4
  %old = atomicrmw add ptr addrspace(1) %s, i32 %k seq_cst, align 4
  %v = load atomic i32, ptr addrspace(1) %s unordered, align 4
  ret i32 %v
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define i32 @t_const
; CHECK-NOT: jeandle.new_instance
; CHECK-NOT: atomicrmw {{[a-z]+ ptr}}
; CHECK-NOT: load atomic
; CHECK: ret i32 13

; CHECK-LABEL: define i32 @t_nonconst
; CHECK-NOT: jeandle.new_instance
; CHECK-NOT: atomicrmw {{[a-z]+ ptr}}
; CHECK-NOT: load atomic
; CHECK: add i32 13, %k
; CHECK: ret i32

!java-method-compilation = !{}
