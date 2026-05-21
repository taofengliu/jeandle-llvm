; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; PEA: verify that the materialization invoke carries the "deopt"
; operand bundle from its source CallBase. The escape point @sink has no
; deopt bundle, so the analyzer falls back to copying the bundle from the
; original allocation invoke.

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
; CHECK: invoke {{.*}}@jeandle.new_instance{{.*}} [ "deopt"(i32 42) ]
; CHECK-NEXT: to label %{{.*}} unwind label %{{.*}}
; CHECK: call void @sink

!java-method-compilation = !{}
