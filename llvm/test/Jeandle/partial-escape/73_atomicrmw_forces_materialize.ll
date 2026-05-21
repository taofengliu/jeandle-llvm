; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; (§8.1.15 / §2.3.3): an atomicrmw on a virtual's field must conservatively
; force materialization. Tier-2 only handles plain Store/Load; AtomicRMWInst
; falls through to materializeAllVirtualOperands. The allocation survives.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare i32 @__gxx_personality_v0(...)

define i32 @test_atomicrmw() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %n unwind label %u
n:
  %s = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  store atomic i32 0, ptr addrspace(1) %s unordered, align 4
  %old = atomicrmw add ptr addrspace(1) %s, i32 5 seq_cst, align 4
  ret i32 %old
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; Allocation must survive in some form (original or materialized). The
; atomicrmw is preserved.
; CHECK-LABEL: define i32 @test_atomicrmw
; CHECK: jeandle.new_instance
; CHECK: atomicrmw add ptr addrspace(1)

!java-method-compilation = !{}
