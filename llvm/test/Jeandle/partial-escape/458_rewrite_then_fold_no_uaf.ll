; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; UAF regression: a folded JavaOp (balanced monitorenter/exit
; on virtual %b) whose monitorenter carries a deopt bundle referencing
; ANOTHER virtual VO (%a). recordDeoptBundleMappings records a
; RewriteDeoptBundleEffect (lower SeqNo) and foldMonitorEnter records a
; ReplaceCallEffect (higher SeqNo) on the SAME call. Pass 1 applies the
; rewrite first: it rebuilds the call and erases the original. Pre-fix, the
; ReplaceCallEffect then dereferenced its freed Target (SIGSEGV). With
; WeakTrackingVH effect targets, the ReplaceCall follows the RAUW to the
; rebuilt call and erases THAT (the fold still fires).

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1), ptr) nounwind
declare hotspotcc void @jeandle.monitorexit_with_lightweight_lock(ptr addrspace(1), ptr) nounwind
declare void @sink(i32)
declare i32 @__gxx_personality_v0(...)

define void @rewrite_then_replace_uaf() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %lk = alloca i64, align 8
  %a = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 11111 to ptr), i32 24)
       to label %na unwind label %u
na:
  %b = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 22222 to ptr), i32 24)
       to label %nb unwind label %u
nb:
  call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(
              ptr addrspace(1) %b, ptr %lk)
       [ "deopt"(i32 0, i32 0, i64 12, ptr addrspace(1) %a) ]
  call void @sink(i32 0)
  call hotspotcc void @jeandle.monitorexit_with_lightweight_lock(
              ptr addrspace(1) %b, ptr %lk)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; Both VOs are NeverEscapes (the bundle died with the folded enter): the
; allocations and both monitor ops are all eliminated. No crash, no poison.
; CHECK-LABEL: define void @rewrite_then_replace_uaf(
; CHECK-NOT: jeandle.new_instance
; CHECK-NOT: monitorenter
; CHECK-NOT: monitorexit
; CHECK: call void @sink(i32 0)
; CHECK: ret void
; CHECK-NOT: poison

!java-method-compilation = !{}
