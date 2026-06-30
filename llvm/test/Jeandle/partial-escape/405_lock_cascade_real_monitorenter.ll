; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; materializeVirtualLocksBefore on a NOT-DELETED monitorenter
; (Graal PartialEscapeClosure.java:263-264, 641-652).
;
; A virtual object %obj holds an elided lock at bytecode depth 0. A REAL
; monitorenter on a non-virtual receiver %otherObj (a parameter) runs at
; depth 1 while %obj's lock is still live. Under strict lock order
; (JeandleAssumeStrictLockOrder defaults true), %obj must materialize BEFORE
; the real monitorenter so its re-emitted lock lands BELOW %otherObj's on the
; lightweight-locking thread lock stack — preserving lexical nesting.
;
; The distinctive assertion is the ORDER: %obj's re-emitted monitorenter must
; precede the real monitorenter(%otherObj). (Distinct from the elide-path
; pre-cascade in 307_pre_cascade_monitorenter_virt.ll, which fires when the
; receiver IS virtual.)

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1), ptr)
declare hotspotcc void @jeandle.monitorexit_with_lightweight_lock(ptr addrspace(1), ptr)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @cascade_real_monitorenter(ptr addrspace(1) %otherObj) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %lo = alloca i64, align 8
  %lpo = alloca i64, align 8
  %obj = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 11111 to ptr), i32 16)
       to label %n1 unwind label %u
n1:
  ; Elided virtual lock on %obj at depth 0.
  call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(
                  ptr addrspace(1) %obj, ptr %lo), !jeandle.lock_depth !{i32 0}
  ; REAL monitorenter on a non-virtual receiver at depth 1 — triggers the
  ; cascade that materializes %obj here.
  call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(
                  ptr addrspace(1) %otherObj, ptr %lpo), !jeandle.lock_depth !{i32 1}
  call hotspotcc void @jeandle.monitorexit_with_lightweight_lock(
                  ptr addrspace(1) %otherObj, ptr %lpo)
  call hotspotcc void @jeandle.monitorexit_with_lightweight_lock(
                  ptr addrspace(1) %obj, ptr %lo)
  call void @sink(ptr addrspace(1) %obj)
  ret void
u:
  %lpad = landingpad i64 cleanup
  resume i64 %lpad
}

; CHECK-LABEL: define void @cascade_real_monitorenter
; %obj materialises, and its re-emitted monitorenter must appear BEFORE the
; real monitorenter(%otherObj).
; CHECK: %[[MAT:[A-Za-z0-9._]+]] = invoke hotspotcc{{.*}}@jeandle.new_instance(ptr inttoptr (i64 11111 to ptr), i32 16)
; CHECK: call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1) %[[MAT]],
; CHECK: call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1) %otherObj,
; CHECK: call void @sink(ptr addrspace(1) %[[MAT]])

!java-method-compilation = !{}
