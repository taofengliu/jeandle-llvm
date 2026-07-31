; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; foldICmpEquality derived-GEP conflation. %o is a virtual
; object; %g = gep %o, 8. Both %o and %g resolve to the same ObjectID
; (resolveVirtualRefImpl's GEP case chases the base and discards the offset).
;
; Before the fix, foldICmpEquality folded `icmp eq ptr %o, ptr %g` to true
; because *V0 == *V1, even though %o and %o+8 are distinct addresses. After the
; fix, the same-ObjectID case also compares resolveFieldOffset: 0 != 8, so the
; fold correctly yields false.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @use(i1)
declare i32 @__gxx_personality_v0(...)

define void @test_icmp_eq_derived_gep() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
  ptr inttoptr (i64 12345 to ptr), i32 32)
  to label %n unwind label %u
n:
  %g = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  %c = icmp eq ptr addrspace(1) %o, %g
  call void @use(i1 %c)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @test_icmp_eq_derived_gep
; The fold must yield false (distinct addresses), not true.
; CHECK: call{{.*}}@use(i1 false)
; CHECK-NOT: call{{.*}}@use(i1 true)

!java-method-compilation = !{}
