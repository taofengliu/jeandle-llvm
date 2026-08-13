; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Receiver of jeandle.check_if_value_based is a NON-virtual oop
; (incoming function argument — no allocation, no virtual to fold against).
; foldCheckIfValueBased's resolveVirtualRef returns nullopt, so the fold
; bails before touching the IsValueBased VMCallback (which is intentionally
; not provided here — no -jeandle-vm-callback-log on the RUN line). The
; runtime check survives unchanged in IR.
;
; This is the "no compile-time klass evidence" conservative path: PEA leaves
; the call alone and the runtime decides at execution time.
;
; Note: per processAllocation, the VObj.Klass == 0 case is unreachable today
; (the analyzer refuses to register a virtual when the alloc's klass operand
; isn't a compile-time constant), so a non-virtual operand is the
; representative test for the "klass unknown" fold path.

declare hotspotcc i1 @jeandle.check_if_value_based(ptr addrspace(1))

declare i32 @__gxx_personality_v0(...)

define i1 @test_value_based_unknown_operand(ptr addrspace(1) %obj) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %r = call hotspotcc i1 @jeandle.check_if_value_based(ptr addrspace(1) %obj)
  ret i1 %r
}

; CHECK-LABEL: define i1 @test_value_based_unknown_operand
; CHECK: %[[R:[A-Za-z0-9._]+]] = call hotspotcc i1 @jeandle.check_if_value_based(ptr addrspace(1) %obj)
; CHECK: ret i1 %[[R]]

!java-method-compilation = !{}
