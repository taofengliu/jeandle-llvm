; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; cmpxchg with a non-constant Expected operand and a constant Current
; slot value is folded into a Conditional pattern (the "Unknown outcome"
; branch). The allocation, the store, and the cmpxchg
; are all eliminated; the cmpxchg is replaced by a synthesized
; { insertvalue / icmp eq } pair packaged into the {i32, i1} result struct.
; The extractvalue chain reads the prior slot value (the constant 0).
; Downstream InstCombine constant-folds extractvalue-of-insertvalue when
; the index path is constant, so the synthesized chain leaves no
; correctness footprint at runtime.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare i32 @__gxx_personality_v0(...)

define i32 @t(i32 %arg) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %n unwind label %u
n:
  %s = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  store atomic i32 0, ptr addrspace(1) %s unordered, align 4
  %p = cmpxchg ptr addrspace(1) %s, i32 %arg, i32 1 seq_cst seq_cst, align 4
  %v = extractvalue { i32, i1 } %p, 0
  ret i32 %v
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define i32 @t
; CHECK-NOT: jeandle.new_instance
; CHECK-NOT: cmpxchg
; CHECK-NOT: store atomic
; CHECK-DAG: icmp eq i32 0, %arg
; CHECK-DAG: insertvalue { i32, i1 } poison, i32 0, 0
; CHECK: ret i32

!java-method-compilation = !{}
