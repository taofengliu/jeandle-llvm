; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; R6.S5 — preserve ALL operand bundles, not only "deopt".
; Before the fix, the bundle-copy loop in applyMaterialize broke after the
; first "deopt" bundle, silently dropping every other tag. Here the escape-
; point sink carries both a "deopt" and a "cfguardtarget" bundle; both must
; survive on the materialisation invoke.

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

; Both bundles must appear on the materialisation invoke.
; CHECK-LABEL: define void @multi_bundle
; CHECK: invoke {{.*}}@jeandle.new_instance{{.*}}[ "deopt"(i32 7), "cfguardtarget"(ptr @cfg_target_fn) ]

!java-method-compilation = !{}
