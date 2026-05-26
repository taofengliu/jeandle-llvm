; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; PEA is intentionally deopt-agnostic until the Jeandle deopt refactor
; lands: the materialization invoke must NOT carry any "deopt" operand
; bundle, regardless of whether the bundle was on the allocation or on
; the escape sink. The DeoptBundleSource field on the analyzer's Effect
; is still set (so the deopt refactor can re-engage it later), but the
; transform drops "deopt" when copying bundles onto NewInv. Here the
; allocation carries a "deopt" bundle and the sink does not; the
; materialization invoke must end up with no operand bundles at all.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_mat_deopt() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       [ "deopt"(i32 42) ]
       to label %n unwind label %u
n:
  call void @sink(ptr addrspace(1) %o)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @test_mat_deopt
; CHECK: %pea.mat = invoke {{.*}}@jeandle.new_instance(ptr {{.*}}, i32 16)
; CHECK-NEXT: to label %{{.*}} unwind label %{{.*}}
; CHECK: call void @sink

!java-method-compilation = !{}
