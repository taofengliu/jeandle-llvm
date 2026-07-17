; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Cyclic field graph (A.f = B, B.g = A) combined with an unbalanced
; monitorenter on A, under the reuse-OrigAlloc model. Returning A escapes the
; whole cascade; both objects stay PARTIALLY-ESCAPING. The ORIGINAL allocations
; (OrigAlloc %a and OrigAlloc %b) are both KEPT (no fresh pea.mat invokes).
; The virtually-tracked field stores are replayed onto the OrigAllocs at the
; single escape point (the return), and the surviving monitorenter stays in
; its original position with receiver OrigAlloc %a — semantically equivalent
; to Graal's CommitAllocationNode lowering (field writes, then MonitorEnters).
; The back edge B.g = A resolves to OrigAlloc %a (no poison).

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc void @jeandle.monitorenter_with_thin_lock(ptr addrspace(1), ptr)
declare i32 @__gxx_personality_v0(...)

define ptr addrspace(1) @cyclic_with_locks_cascade()
    gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %lk1 = alloca i64, align 8
  %a = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 11111 to ptr), i32 16) to label %na unwind label %u
na:
  %b = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 22222 to ptr), i32 16) to label %nb unwind label %u
nb:
  %sa = getelementptr inbounds i8, ptr addrspace(1) %a, i64 8
  store atomic ptr addrspace(1) %b, ptr addrspace(1) %sa unordered, align 8
  %sb = getelementptr inbounds i8, ptr addrspace(1) %b, i64 8
  store atomic ptr addrspace(1) %a, ptr addrspace(1) %sb unordered, align 8
  call hotspotcc void @jeandle.monitorenter_with_thin_lock(ptr addrspace(1) %a, ptr %lk1)
  ret ptr addrspace(1) %a
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

!java-method-compilation = !{}

; CHECK-LABEL: define ptr addrspace(1) @cyclic_with_locks_cascade
; Both ORIGINAL allocation invokes are RETAINED (no fresh materialization).
; CHECK: %a = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 11111 to ptr), i32 16)
; CHECK: %b = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 22222 to ptr), i32 16)
; No pea.mat materialization invoke is emitted.
; CHECK-NOT: pea.mat = invoke
; Both cyclic field stores are replayed onto OrigAllocs at the escape point,
; with each back edge resolving to the correct OrigAlloc (no poison).
; CHECK: store atomic ptr addrspace(1) %a, ptr addrspace(1) %pea.matslot{{[0-9]*}} unordered, align 8
; CHECK: store atomic ptr addrspace(1) %b, ptr addrspace(1) %pea.matslot{{[0-9]*}} unordered, align 8
; CHECK-NOT: poison
; The surviving monitorenter stays in its original position, receiver OrigAlloc %a.
; CHECK: call hotspotcc void @jeandle.monitorenter_with_thin_lock(ptr addrspace(1) %a, ptr %lk1)
; CHECK: ret ptr addrspace(1) %a
