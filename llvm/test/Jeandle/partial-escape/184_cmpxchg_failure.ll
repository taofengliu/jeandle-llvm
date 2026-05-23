; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; B17: cmpxchg failure path. Expected does NOT match the slot's current
; entry, so the comparison folds to false; the slot stays unchanged; the
; cmpxchg's result struct is replaced with the constant `{prior, false}`.
; A subsequent load sees the unchanged (prior) value.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare i32 @__gxx_personality_v0(...)

define i32 @t() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %n unwind label %u
n:
  %s = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  store atomic i32 7, ptr addrspace(1) %s unordered, align 4
  %p = cmpxchg ptr addrspace(1) %s, i32 0, i32 1 seq_cst seq_cst, align 4
  %prior = extractvalue { i32, i1 } %p, 0
  %ok    = extractvalue { i32, i1 } %p, 1
  %after = load atomic i32, ptr addrspace(1) %s unordered, align 4
  %ok32 = zext i1 %ok to i32
  %t0 = add i32 %prior, %ok32
  %t1 = add i32 %t0, %after
  ret i32 %t1
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; The slot was 7 and stays 7 because the CAS failed. The result struct is
; `{ i32 7, i1 false }`; the post-CAS load sees 7.
; CHECK-LABEL: define i32 @t
; CHECK-NOT: jeandle.new_instance
; CHECK-NOT: cmpxchg
; CHECK-NOT: store atomic
; CHECK-NOT: load atomic
; CHECK: extractvalue { i32, i1 } { i32 7, i1 false }, 0
; CHECK: extractvalue { i32, i1 } { i32 7, i1 false }, 1
; CHECK: ret i32

!java-method-compilation = !{}
