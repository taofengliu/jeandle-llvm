; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; B17: atomicrmw add with constant current + constant operand → constant
; fold. Slot starts as 10 (from the store), atomicrmw add 5 returns the
; prior 10 and sets the slot to 15. Returning the "prior" produces
; `ret i32 10`.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare i32 @__gxx_personality_v0(...)

define i32 @t() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %n unwind label %u
n:
  %s = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  store atomic i32 10, ptr addrspace(1) %s unordered, align 4
  %old = atomicrmw add ptr addrspace(1) %s, i32 5 seq_cst, align 4
  ret i32 %old
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define i32 @t
; CHECK-NOT: jeandle.new_instance
; CHECK-NOT: atomicrmw
; CHECK: ret i32 10

!java-method-compilation = !{}
