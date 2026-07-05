; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Select of equal-offset GEPs into a virtual object (review #1.3).
; %g1 and %g2 both offset %o by 16; %sel = select %c, %g1, %g2; the load is
; through %sel, whose runtime address is %o+16 (field @16 = 42).
;
; Before the fix, propagatePointerAlias alias-forwarded %sel to %o's ObjectID
; with no offset guard (resolveVirtualRefImpl's Select case returns the common
; ObjectID and discards per-arm offset). resolveFieldOffset(%sel) then returned
; 0 (stripPointerCastsAndOffsets has no Select case), so processLoad modelled
; the load at field @0 (= 7) instead of @16 (= 42) -> a miscompile, not a
; conservative bail. After the fix, a Select with any non-zero-offset arm falls
; through to materializeAllVirtualOperands: %o materializes at the select and
; the load reads the real %o+16 address.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @use(i32)
declare i32 @__gxx_personality_v0(...)

define void @test_select_equal_offset_gep(i1 %c) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 32)
         to label %n unwind label %u
n:
  %f0 = getelementptr inbounds i8, ptr addrspace(1) %o, i64 0
  %g1 = getelementptr inbounds i8, ptr addrspace(1) %o, i64 16
  %g2 = getelementptr inbounds i8, ptr addrspace(1) %o, i64 16
  store atomic i32 7, ptr addrspace(1) %f0 unordered, align 4
  store atomic i32 42, ptr addrspace(1) %g1 unordered, align 4
  %sel = select i1 %c, ptr addrspace(1) %g1, ptr addrspace(1) %g2
  %r = load atomic i32, ptr addrspace(1) %sel unordered, align 4
  call void @use(i32 %r)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @test_select_equal_offset_gep
; The object materializes (the select bail forces it) and the load survives as
; a real load through %sel, reading %o+16. The load must NOT be folded to the
; field@0 value 7.
; CHECK: invoke{{.*}}@jeandle.new_instance
; CHECK: load atomic i32, ptr addrspace(1) %sel
; CHECK-NOT: call{{.*}}@use(i32 7)

!java-method-compilation = !{}
