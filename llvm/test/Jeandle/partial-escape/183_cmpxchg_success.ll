; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; B17: cmpxchg success path. Expected matches the slot's current entry,
; so the comparison folds to true; the slot is updated to the NewVal; the
; cmpxchg's result struct is replaced with the constant `{prior, true}`.
; A subsequent load sees the new value.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare i32 @__gxx_personality_v0(...)

define i32 @t() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %n unwind label %u
n:
  %s = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  store atomic i32 42, ptr addrspace(1) %s unordered, align 4
  %p = cmpxchg ptr addrspace(1) %s, i32 42, i32 99 seq_cst seq_cst, align 4
  %prior = extractvalue { i32, i1 } %p, 0
  %ok    = extractvalue { i32, i1 } %p, 1
  %after = load atomic i32, ptr addrspace(1) %s unordered, align 4
  ; Sum of (prior=42) + (ok=1) + (after=99) = 142.
  %ok32 = zext i1 %ok to i32
  %t0 = add i32 %prior, %ok32
  %t1 = add i32 %t0, %after
  ret i32 %t1
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; The cmpxchg and the surrounding store / load on the virtual slot are all
; eliminated; the cmpxchg's result is replaced with the constant struct
; `{ i32 42, i1 true }` (extractvalue chains survive but constant-fold via
; downstream InstSimplify). The `after` load sees the post-CAS slot value 99.
; CHECK-LABEL: define i32 @t
; CHECK-NOT: jeandle.new_instance
; CHECK-NOT: cmpxchg
; CHECK-NOT: store atomic
; CHECK-NOT: load atomic
; CHECK: extractvalue { i32, i1 } { i32 42, i1 true }, 0
; CHECK: extractvalue { i32, i1 } { i32 42, i1 true }, 1
; CHECK: add i32 {{.*}}, 99
; CHECK: ret i32

!java-method-compilation = !{}
