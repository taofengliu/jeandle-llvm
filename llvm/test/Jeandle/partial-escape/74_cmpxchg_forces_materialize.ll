; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; cmpxchg on a virtual's field must conservatively force materialization.
; AtomicCmpXchgInst is not a recognised tier-2 shape; it falls through to
; materializeAllVirtualOperands.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare i32 @__gxx_personality_v0(...)

define i32 @test_cmpxchg() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %n unwind label %u
n:
  %s = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  store atomic i32 0, ptr addrspace(1) %s unordered, align 4
  %p = cmpxchg ptr addrspace(1) %s, i32 0, i32 1 seq_cst seq_cst, align 4
  %v = extractvalue { i32, i1 } %p, 0
  ret i32 %v
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define i32 @test_cmpxchg
; CHECK: jeandle.new_instance
; CHECK: cmpxchg ptr addrspace(1)

!java-method-compilation = !{}
