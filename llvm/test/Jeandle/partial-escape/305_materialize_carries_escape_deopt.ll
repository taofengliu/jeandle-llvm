; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; DeoptBundleSource prefers the escape-point CallBase when it carries a
; "deopt" bundle. The analyzer's DeoptBundleSource still reflects this
; preference (so the future Jeandle deopt refactor can engage with the
; bundle properly), but the transform is intentionally deopt-agnostic:
; it filters "deopt" out when copying bundles onto NewInv. So even
; though the sink carries a rich "deopt" bundle and the allocation has
; none, the materialization invoke must end up with no operand bundles
; at all, and the sink's "deopt" operand referencing the
; about-to-be-deleted OrigAlloc must be scrubbed to null before the
; global RAUW (otherwise the verifier would see a self-reference once
; OrigAlloc is replaced by NewInv).

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
; CHECK: %pea.mat = invoke {{.*}}@jeandle.new_instance(ptr {{.*}}, i32 16)
; CHECK-NEXT: to label %{{.*}} unwind label %{{.*}}

!java-method-compilation = !{}
