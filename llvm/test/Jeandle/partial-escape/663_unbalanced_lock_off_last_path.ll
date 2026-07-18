; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; An elided monitorenter with no matching exit on a path whose return block
; is NOT the last block in RPO. The commit-time unbalanced-lock gate must
; consult the per-block EXIT SNAPSHOTS of every return/resume block (not the
; analyzer's live lock map, which only reflects the last processed block),
; or the fold survives on this path: the method would return normally where
; the real runtime throws IllegalMonitorStateException for the unbalanced
; monitor.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1), ptr)
declare void @sink_i32(i32)
declare i32 @__gxx_personality_v0(...)

define void @unbalanced_enter_off_last_path(i1 %c) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %lo = alloca i64, align 8
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 9103 to ptr), i32 16)
       to label %n unwind label %u
n:
  br i1 %c, label %early_ret, label %more
early_ret:
  call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(
                  ptr addrspace(1) %o, ptr %lo)
  ret void
more:
  call void @sink_i32(i32 0)
  call void @sink_i32(i32 1)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; The unbalanced enter keeps the object real: the allocation AND the
; monitorenter survive.
; CHECK-LABEL: define void @unbalanced_enter_off_last_path
; CHECK: %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance
; CHECK: call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1) %o,

!java-method-compilation = !{}
