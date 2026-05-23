; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; R8.M11: a constant-condition branch's dead arm contributes nothing to
; downstream merges / PHI fan-in. Without dead-edge tracking, processing
; the unreachable arm's sink call would materialize the virtual (because
; sink is an opaque escape consumer), preventing alloc elimination. With
; R8.M11, PEA skips the dead block entirely and the virtual stays
; eliminable on the live path.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare void @use(i32)
declare i32 @__gxx_personality_v0(...)

define void @test_dead_pred_const(i32 %x)
    gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %prep unwind label %u
prep:
  %s = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  store atomic i32 7, ptr addrspace(1) %s unordered, align 4
  ; Constant condition: only %live executes.
  br i1 false, label %dead, label %live
dead:
  ; This sink would normally escape %o; PEA must NOT materialize on this
  ; statically-unreachable path.
  call void @sink(ptr addrspace(1) %o)
  br label %fast
live:
  %v = load atomic i32, ptr addrspace(1) %s unordered, align 4
  call void @use(i32 %v)
  br label %fast
fast:
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; The alloc and stores/loads on the live path are fully eliminated; the
; sink call on the dead arm survives in IR (its receiver becomes poison
; after EliminateAllocation RAUWs %o → poison) until a downstream pass
; reaps the unreachable block.
; CHECK-LABEL: define void @test_dead_pred_const
; CHECK-NOT: jeandle.new_instance
; CHECK-NOT: store atomic
; CHECK-NOT: load atomic
; CHECK: call void @use(i32 7)

!java-method-compilation = !{}
