; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Invoke-unwind lock re-emit vs the unwind-edge state split.
; A locked VO escapes at a TERMINATOR invoke `foo(o)`. The materialize is
; placed BEFORE the invoke, so the re-emitted monitorenter executes on BOTH
; the normal and unwind edges. Pre-fix, the unwind successor inherited the
; PRE-invoke snapshot (VO still virtual+locked), so the handler's escape
; re-emitted the same enter AGAIN (double acquire) and the unwind path had
; no matching exit. The UnwindData patch now records the VO as materialized
; (locks cleared — the re-emit already covers them) in the pre-invoke
; snapshot: the handler sees the real object, no second re-emit, and the
; normal path's monitorexit survives.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1), ptr)
declare hotspotcc void @jeandle.monitorexit_with_lightweight_lock(ptr addrspace(1), ptr)
declare void @foo(ptr addrspace(1))
declare void @bar(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @invoke_escape(i1 %c) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %lo = alloca i64, align 8
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 11111 to ptr), i32 16)
       to label %n unwind label %u
n:
  call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(
              ptr addrspace(1) %o, ptr %lo)
  invoke void @foo(ptr addrspace(1) %o) to label %ok unwind label %handler
ok:
  call hotspotcc void @jeandle.monitorexit_with_lightweight_lock(
              ptr addrspace(1) %o, ptr %lo)
  ret void
handler:
  %lp = landingpad i64 cleanup
  ; o escapes in the handler too
  call void @bar(ptr addrspace(1) %o)
  resume i64 %lp
u:
  %lpr = landingpad i64 cleanup
  resume i64 %lpr
}

; Exactly ONE re-emitted enter (immediately before the invoke, covering both
; edges); the ok path's exit survives as a real exit; the handler's bar(o)
; gets NO second re-emit.
; CHECK-LABEL: define void @invoke_escape(
; CHECK: call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1) %o, ptr %lo)
; CHECK-NEXT: invoke void @foo(ptr addrspace(1) %o)
; CHECK-NOT: monitorenter
; CHECK: call hotspotcc void @jeandle.monitorexit_with_lightweight_lock(ptr addrspace(1) %o, ptr %lo)
; CHECK: handler:
; CHECK: call void @bar(ptr addrspace(1) %o)
; CHECK-NOT: monitorenter
; CHECK-NOT: poison

!java-method-compilation = !{}
