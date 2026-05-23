; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; R6.S4 — Materialised invoke preserves metadata and function/arg attrs of
; the original allocation. Before R6.S4, applyMaterialize only carried over
; the calling convention + three hard-coded return attrs (JavaKlass /
; JavaKlassExact / NonNull); !prof, !alias.scope, !noalias, custom Jeandle
; metadata, and function-level attrs (nofree, nosync, cold) were dropped.
;
; The test forces a materialisation by passing the virtual to an opaque
; sink, then asserts the materialisation invoke still carries the
; frontend-attached !prof and !alias.scope metadata from the original alloc.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @meta_preserved() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %n unwind label %u, !prof !0, !alias.scope !1
n:
  call void @sink(ptr addrspace(1) %o)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @meta_preserved
; The materialised invoke must carry forward the !prof and !alias.scope
; metadata that the frontend attached to the original allocation.
; CHECK: invoke {{.*}}@jeandle.new_instance
; CHECK-NEXT: to label %{{.*}} unwind label %{{.*}}, !prof !{{[0-9]+}}, !alias.scope !{{[0-9]+}}

!java-method-compilation = !{}
!0 = !{!"branch_weights", i32 100}
!1 = !{!2}
!2 = distinct !{!2, !3, !"jeandle.scope.0"}
!3 = distinct !{!3, !"jeandle.scope.domain"}
