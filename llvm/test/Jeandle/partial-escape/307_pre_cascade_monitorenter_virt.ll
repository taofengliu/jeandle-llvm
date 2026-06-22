; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; materializeVirtualLocksBefore pre-cascade.
;
; Two virtuals A and B, both entered (virtual monitorenter). At sink(%a),
; the narrow lock-cascade rule (other.minOrder < this.maxOrder) would
; observe Order(B).front=1 vs A.back=0 → 1<0 false → no cascade of B at
; A's escape. Without the pre-cascade at foldMonitorEnter, B would only
; materialise at sink(%b), so the lock-stack observable from sink(%a)
; would silently lack B's lock — a Java-semantics change.
;
; At foldMonitorEnter(B) we pre-cascade A (because A.front=0 <
; B.NewOrder=1) so A materialises BEFORE B's virtual lock is added. The
; result is that both monitorenters appear in IR before sink(%a), and the
; runtime lock stack at sink(%a) is correctly [A, B].

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1), ptr)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @pre_cascade() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %la = alloca i64, align 8
  %lb = alloca i64, align 8
  %a = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 11111 to ptr), i32 16)
       to label %n1 unwind label %u
n1:
  call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(
                  ptr addrspace(1) %a, ptr %la)
  %b = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 22222 to ptr), i32 16)
       to label %n2 unwind label %u
n2:
  ; Virtual monitorenter on B — fires the pre-cascade of A.
  call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(
                  ptr addrspace(1) %b, ptr %lb)
  ; Escape A first.
  call void @sink(ptr addrspace(1) %a)
  ; Escape B later.
  call void @sink(ptr addrspace(1) %b)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; Both A and B must be materialised. B's allocation invoke MUST appear
; before sink(%a) — the marker that pre-cascade fired and B was hoisted
; into IR alongside A.
; CHECK-LABEL: define void @pre_cascade
; CHECK: %[[MATA:[A-Za-z0-9._]+]] = invoke hotspotcc{{.*}}@jeandle.new_instance(ptr inttoptr (i64 11111 to ptr), i32 16)
; CHECK: call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1) %[[MATA]],
; CHECK: %[[MATB:[A-Za-z0-9._]+]] = invoke hotspotcc{{.*}}@jeandle.new_instance(ptr inttoptr (i64 22222 to ptr), i32 16)
; CHECK: call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1) %[[MATB]],
; CHECK: call void @sink(ptr addrspace(1) %[[MATA]])
; CHECK: call void @sink(ptr addrspace(1) %[[MATB]])

!java-method-compilation = !{}
