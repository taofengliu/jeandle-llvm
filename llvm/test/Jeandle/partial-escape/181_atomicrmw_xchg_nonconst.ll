; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; B17: atomicrmw xchg with a non-constant value still folds. Xchg's new
; entry IS the operand (no binop required), so the slot is updated to
; %arg and the atomicrmw RAUW's to the prior value. A later load of the
; slot sees %arg. The function returns prior + new = 7 + %arg.

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
  %old = atomicrmw xchg ptr addrspace(1) %s, i32 %arg seq_cst, align 4
  %new = load atomic i32, ptr addrspace(1) %s unordered, align 4
  %sum = add i32 %old, %new
  ret i32 %sum
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define i32 @t
; CHECK-NOT: jeandle.new_instance
; CHECK-NOT: atomicrmw
; CHECK-NOT: load atomic
; CHECK: %sum = add i32 7, %arg
; CHECK: ret i32 %sum

!java-method-compilation = !{}
