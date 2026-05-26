; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; atomicrmw on a slot that was NEVER stored — the field-state lookup
; misses, and tier2AtomicRMW (mirroring tier2Load's default-zero semantics)
; treats the prior value as the type's zero. `atomicrmw add 5` then folds
; against 0, returns the prior (0), and updates the slot to 5.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare i32 @__gxx_personality_v0(...)

define i32 @t() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %n unwind label %u
n:
  %s = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  ; No prior store; field starts as default-zero.
  %old = atomicrmw add ptr addrspace(1) %s, i32 5 seq_cst, align 4
  ret i32 %old
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define i32 @t
; CHECK-NOT: jeandle.new_instance
; CHECK-NOT: atomicrmw
; CHECK: ret i32 0

!java-method-compilation = !{}
