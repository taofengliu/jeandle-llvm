; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Exercise the full supported atomicrmw op set against one virtual:
; xchg, add, sub, and, or, xor. Each op stores a constant first, then
; performs the op with a constant operand. The returned prior values
; should compile-time-fold and the allocation is eliminated.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare i32 @__gxx_personality_v0(...)

define i32 @t() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 64)
       to label %n unwind label %u
n:
  %p0 = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  %p1 = getelementptr inbounds i8, ptr addrspace(1) %o, i64 16
  %p2 = getelementptr inbounds i8, ptr addrspace(1) %o, i64 24
  %p3 = getelementptr inbounds i8, ptr addrspace(1) %o, i64 32
  %p4 = getelementptr inbounds i8, ptr addrspace(1) %o, i64 40
  %p5 = getelementptr inbounds i8, ptr addrspace(1) %o, i64 48
  store atomic i32 100, ptr addrspace(1) %p0 unordered, align 4
  store atomic i32 200, ptr addrspace(1) %p1 unordered, align 4
  store atomic i32 300, ptr addrspace(1) %p2 unordered, align 4
  store atomic i32 15,  ptr addrspace(1) %p3 unordered, align 4
  store atomic i32 5,   ptr addrspace(1) %p4 unordered, align 4
  store atomic i32 6,   ptr addrspace(1) %p5 unordered, align 4
  %x  = atomicrmw xchg ptr addrspace(1) %p0, i32 1  seq_cst, align 4
  %a  = atomicrmw add  ptr addrspace(1) %p1, i32 2  seq_cst, align 4
  %s  = atomicrmw sub  ptr addrspace(1) %p2, i32 3  seq_cst, align 4
  %an = atomicrmw and  ptr addrspace(1) %p3, i32 12 seq_cst, align 4
  %or = atomicrmw or   ptr addrspace(1) %p4, i32 2  seq_cst, align 4
  %xr = atomicrmw xor  ptr addrspace(1) %p5, i32 3  seq_cst, align 4
  %t0 = add i32 %x,  %a
  %t1 = add i32 %t0, %s
  %t2 = add i32 %t1, %an
  %t3 = add i32 %t2, %or
  %t4 = add i32 %t3, %xr
  ret i32 %t4
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; All six atomicrmws are folded; the original alloc + stores are dropped;
; each prior value appears as a constant in the returned sum.
; CHECK-LABEL: define i32 @t
; CHECK-NOT: jeandle.new_instance
; CHECK-NOT: atomicrmw
; CHECK-NOT: store atomic
; CHECK: add i32 100, 200
; CHECK: add i32 {{.*}}, 300
; CHECK: add i32 {{.*}}, 15
; CHECK: add i32 {{.*}}, 5
; CHECK: add i32 {{.*}}, 6
; CHECK: ret i32

!java-method-compilation = !{}
