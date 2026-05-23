; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; R11.V28: variant exercising the InsertValueInst chain for the {T, i1}
; result struct. With non-constant expected, both the prior-value extract
; and the success-bit extract land on the synthesized struct. The bit-
; level shape after fold:
;   %eq  = icmp eq i32 5, %x
;   %s0  = insertvalue {i32,i1} poison, i32 5, 0
;   %s1  = insertvalue {i32,i1} %s0, i1 %eq, 1
;   %p1  = extractvalue {i32,i1} %s1, 0  ; prior
;   %p2  = extractvalue {i32,i1} %s1, 1  ; success bit
; Both extractvalues should constant-fold-eligible (S0 covers index 0)
; even when later stages run InstCombine, but the lit run only exercises
; PEA, so the structural shape is what we check.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare i32 @__gxx_personality_v0(...)

define i32 @t(i32 %x) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %n unwind label %u
n:
  %s = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  store atomic i32 5, ptr addrspace(1) %s unordered, align 4
  %p = cmpxchg ptr addrspace(1) %s, i32 %x, i32 42 seq_cst seq_cst, align 4
  %prior = extractvalue { i32, i1 } %p, 0
  %ok = extractvalue { i32, i1 } %p, 1
  %ok32 = zext i1 %ok to i32
  %sum = add i32 %prior, %ok32
  ret i32 %sum
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define i32 @t
; CHECK-NOT: jeandle.new_instance
; CHECK-NOT: cmpxchg
; CHECK-NOT: store atomic
; CHECK-DAG: icmp eq i32 5, %x
; CHECK-DAG: insertvalue { i32, i1 } poison, i32 5, 0
; CHECK-DAG: insertvalue { i32, i1 } %{{.*}}, i1 %{{.*}}, 1
; CHECK: ret i32

!java-method-compilation = !{}
