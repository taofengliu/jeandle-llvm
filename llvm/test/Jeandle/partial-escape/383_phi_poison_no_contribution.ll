; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Poison incoming on a Select arm is treated as no-contribution by
; resolveVirtualRef. A `select i1 %c, ptr alloc, ptr poison` resolves to
; the alloc — the poison-arm path would be UB if executed. PEA's
; resolveVirtualRefImpl Select handler ignores poison arms so the load
; through the select folds against the virtual.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare i32 @__gxx_personality_v0(...)

define i32 @t(i1 %c) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %n unwind label %u
n:
  %s0 = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  store atomic i32 77, ptr addrspace(1) %s0 unordered, align 4
  ; Select between virtual and poison. resolveVirtualRef must see through
  ; to the virtual.
  %sel = select i1 %c, ptr addrspace(1) %o, ptr addrspace(1) poison
  %s1 = getelementptr inbounds i8, ptr addrspace(1) %sel, i64 8
  %v = load atomic i32, ptr addrspace(1) %s1 unordered, align 4
  ret i32 %v
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define i32 @t
; CHECK-NOT: jeandle.new_instance
; CHECK-NOT: store atomic
; CHECK-NOT: load atomic
; CHECK: ret i32 77

!java-method-compilation = !{}
