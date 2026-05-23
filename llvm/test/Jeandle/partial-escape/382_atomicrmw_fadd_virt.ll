; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; R11.R19: atomicrmw fadd / fsub / fmin / fmax on a virtual slot with
; constant operands fold at compile time. The ConstantFoldBinaryOpOperands
; path handles fadd/fsub; APFloat::minnum/maxnum drives fmin/fmax.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare i32 @__gxx_personality_v0(...)

define double @test_fadd() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %n unwind label %u
n:
  %s = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  store atomic double 1.0, ptr addrspace(1) %s unordered, align 8
  %old = atomicrmw fadd ptr addrspace(1) %s, double 2.5 seq_cst, align 8
  ; %old should be the prior value (1.0); slot is now 3.5.
  %now = load atomic double, ptr addrspace(1) %s unordered, align 8
  %sum = fadd double %old, %now
  ret double %sum
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define double @test_fadd
; CHECK-NOT: jeandle.new_instance
; CHECK-NOT: atomicrmw
; CHECK-NOT: store atomic
; CHECK-NOT: load atomic
; PEA folds the atomicrmw + store + load to constants 1.0 (prior) and 3.5
; (post). Constant fadd is not done by PEA itself; downstream InstCombine
; would reduce this to a constant. We just check the operand shape.
; CHECK: fadd double 1.000000e+00, 3.500000e+00
; CHECK: ret double

!java-method-compilation = !{}
