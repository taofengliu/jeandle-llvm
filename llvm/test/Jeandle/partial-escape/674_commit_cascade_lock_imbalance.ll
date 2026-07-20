; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s
; RUN: opt -disable-output -passes="require<partial-escape-analysis>" -jeandle-dump-pea-stats %s 2>&1 | FileCheck %s --check-prefix=STATS

; Commit-time VirtualRefEdges ineligibility cascade —
; backstop coverage for NON-store ineligibility. Since unvirtualizable
; stores now materialize their operands AT the store (recursively
; materializing nested VirtualRefs, MatReason::Nested), the commit-time
; cascade over the persistent VirtualRefEdges set only fires for objects
; made ineligible by paths that have no materialize point: unbalanced
; locking at a function exit, merge hazards, the availability sweep, and
; the deopt-descriptor cascade. This test covers the first of those.
;
; A virtual OUTER holds a tracked VirtualRef field to a virtual INNER:
; outer.f = inner records FieldStates[outer][16] = VirtualRef(inner), an
; EliminateStoreEffect, and the persistent edge outer->inner. A folded
; monitorenter on OUTER is never balanced: at the function exit
; LockCounts[outer] != 0, so commit() marks OUTER ineligible — the lock
; fold is revoked and OUTER's EliminateStoreEffect is dropped, leaving the
; field store in place as a REAL store. The cascade (commit() producer (a))
; must then drag INNER to ineligible too: otherwise INNER is classified
; NeverEscapes and Pass 2 RAUWs inner.OrigAlloc to poison, so the surviving
; real store would write `store ptr poison` into the real OUTER.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1), ptr)
declare i32 @__gxx_personality_v0(...)

define void @commit_cascade_lock_imbalance() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %lo = alloca i64, align 8
  %inner = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
               ptr inttoptr (i64 9201 to ptr), i32 24)
           to label %n1 unwind label %u
n1:
  %outer = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
               ptr inttoptr (i64 9202 to ptr), i32 24)
           to label %n2 unwind label %u
n2:
  %f = getelementptr inbounds i8, ptr addrspace(1) %outer, i64 16
  store atomic ptr addrspace(1) %inner, ptr addrspace(1) %f unordered, align 4
  call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(
                  ptr addrspace(1) %outer, ptr %lo)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; Both allocations survive (AlwaysEscapes), the store survives verbatim
; writing the real inner pointer, the unbalanced monitorenter survives, and
; nothing is poisoned.
; CHECK-LABEL: define void @commit_cascade_lock_imbalance
; CHECK: %inner = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance
; CHECK: %outer = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance
; CHECK: store atomic ptr addrspace(1) %inner, ptr addrspace(1) %f unordered, align 4
; CHECK: call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1) %outer,
; CHECK-NOT: poison

; The commit-time cascade fires: BOTH objects classify AlwaysEscapes.
; STATS: ;; PEA stats @commit_cascade_lock_imbalance: NeverEscapes=0 PartiallyEscapes=0 AlwaysEscapes=2

!java-method-compilation = !{}
