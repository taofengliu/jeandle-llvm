; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; R6.S10 — DeoptBundleSource prefers the escape-point CallBase when it
; carries a "deopt" bundle. Before the fix, materializeAt unconditionally
; recorded VObj.AllocationCall as the bundle source, so a rich "deopt"
; bundle attached to the sink call was silently dropped on the
; materialization invoke. Post-fix: when the escape-point InsertBefore is a
; CallBase with a "deopt" bundle, materializeAt records it as the
; DeoptBundleSource so the materialization invoke carries the sink's
; bundle to downstream RewriteStatepointsForGC.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @escape_with_bundle() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  ; No "deopt" bundle on the allocation itself.
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %n unwind label %u
n:
  ; Rich "deopt" bundle on the sink — this is what must be carried over.
  call void @sink(ptr addrspace(1) %o) [ "deopt"(i32 42) ]
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @escape_with_bundle
; CHECK: invoke {{.*}}@jeandle.new_instance{{.*}} [ "deopt"(i32 42) ]

!java-method-compilation = !{}
