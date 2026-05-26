; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; R6.S5 — preserve all non-"deopt" operand bundles. PEA is intentionally
; deopt-agnostic until the Jeandle deopt refactor lands (the transform
; drops "deopt" when copying bundles onto NewInv), but every other tag
; must survive. Here the allocation carries both a "deopt" and a
; "cfguardtarget" bundle; only the "cfguardtarget" bundle survives on
; the materialisation invoke. This still catches the original R6.S5 bug
; (a bundle-copy loop that broke after the first bundle, silently
; dropping subsequent ones).

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare void @cfg_target_fn()
declare i32 @__gxx_personality_v0(...)

define void @multi_bundle() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       [ "deopt"(i32 7), "cfguardtarget"(ptr @cfg_target_fn) ]
       to label %n unwind label %u
n:
  call void @sink(ptr addrspace(1) %o)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; The non-"deopt" bundle must survive on the materialisation invoke;
; the "deopt" bundle must be dropped.
; CHECK-LABEL: define void @multi_bundle
; CHECK: invoke {{.*}}@jeandle.new_instance{{.*}}[ "cfguardtarget"(ptr @cfg_target_fn) ]
; CHECK-NOT: "deopt"

!java-method-compilation = !{}
