; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; cmpxchg with a non-constant Expected operand and a constant
; current slot value is folded into a Conditional pattern (icmp + select),
; mirroring Graal AbstractUnsafeCompareAndSwapNode.virtualize lines
; 117-189. The slot's post-CAS value becomes
; `select(eq(prior, expected), newVal, prior)` so a subsequent atomic load
; of the slot sees that select. The cmpxchg's {T, i1} result is built via
; an InsertValueInst chain.
;
; Test shape: alloc, store i32 7 at slot+8, then cmpxchg with %arg as
; expected and 99 as newVal. The non-constant compare prevents a
; static fold but PEA still virtualizes — the alloc, store, and cmpxchg
; are all eliminated.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare i32 @__gxx_personality_v0(...)

define i32 @t(i32 %arg) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %n unwind label %u
n:
  %s = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  store atomic i32 7, ptr addrspace(1) %s unordered, align 4
  %p = cmpxchg ptr addrspace(1) %s, i32 %arg, i32 99 seq_cst seq_cst, align 4
  %prior = extractvalue { i32, i1 } %p, 0
  %ok = extractvalue { i32, i1 } %p, 1
  %ok32 = zext i1 %ok to i32
  ; Read the slot post-CAS — must observe `select(eq(7, %arg), 99, 7)`.
  %after = load atomic i32, ptr addrspace(1) %s unordered, align 4
  %t0 = add i32 %prior, %ok32
  %t1 = add i32 %t0, %after
  ret i32 %t1
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define i32 @t
; CHECK-NOT: jeandle.new_instance
; CHECK-NOT: cmpxchg
; CHECK-NOT: store atomic
; CHECK-NOT: load atomic
; The synthesized icmp and select must be present (non-constant fold path).
; CHECK-DAG: icmp eq i32 7, %arg
; CHECK-DAG: select i1 {{.*}}, i32 99, i32 7
; CHECK: ret i32

!java-method-compilation = !{}
