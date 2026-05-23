; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; PEA mixed-state merge: branch %left escapes the object via a sink call,
; the other branch keeps it virtual until the merge. The analyzer inherits
; Materialized at the merge using the OrigAlloc placeholder. The transform's
; safe-IP-hoisted materializeAt produces a single materialized invoke that
; dominates the merge; downstream uses snap to it via RAUW.
;
; A more aggressive design — per-pred materialization + a synthesized
; ptr addrspace(1) PHI at the merge — would require a DT-aware fix-up to
; redirect downstream uses through the PHI. That's deferred to a future task.
; This test only asserts that the merge no longer bails to ineligible.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define ptr addrspace(1) @test_mixed_merge(i1 %c)
    gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %n unwind label %u
n:
  br i1 %c, label %left, label %right
left:
  call void @sink(ptr addrspace(1) %o)
  br label %merge
right:
  br label %merge
merge:
  ret ptr addrspace(1) %o
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define ptr addrspace(1) @test_mixed_merge
; CHECK: %[[MAT:[A-Za-z0-9._]+]] = invoke hotspotcc{{.*}}@jeandle.new_instance
; CHECK: call void @sink(ptr addrspace(1) %[[MAT]])
; CHECK: ret ptr addrspace(1) %[[MAT]]

!java-method-compilation = !{}
