; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" -jeandle-vm-callback-log=%S/Inputs/263_box_case_c_merge.cblog %s | FileCheck %s

; B10 Phase 4 (exercise / smoke test): if-else autobox of two values
; merges via a PHI; the post-merge unbox load resolves against the
; synthetic Case-C VO. Both per-pred allocations and stores must
; disappear and the load must collapse to a PHI of the two boxed
; primitives.
;
; Klass 9999 is Integer (JBasicType::Int=4); both per-pred VOs carry
; BoxedPrimitiveKind == 4 so the Phase 4 identity-bail-drop path applies
; (though this particular test does not have an external use of either
; alloc — the merge is also legal on the non-boxed Case-C path). The
; primary signal is that the merged VO continues to be virtual through
; the unbox load.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)

declare i32 @__gxx_personality_v0(...)

define i32 @test_box_case_c(i1 %c, i32 %x, i32 %y)
    gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  br i1 %c, label %left, label %right
left:
  %o1 = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 9999 to ptr), i32 16)
        to label %lstore unwind label %u
lstore:
  %sl = getelementptr inbounds i8, ptr addrspace(1) %o1, i64 12
  store atomic i32 %x, ptr addrspace(1) %sl unordered, align 4
  br label %merge
right:
  %o2 = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 9999 to ptr), i32 16)
        to label %rstore unwind label %u
rstore:
  %sr = getelementptr inbounds i8, ptr addrspace(1) %o2, i64 12
  store atomic i32 %y, ptr addrspace(1) %sr unordered, align 4
  br label %merge
merge:
  %p = phi ptr addrspace(1) [ %o1, %lstore ], [ %o2, %rstore ]
  %sm = getelementptr inbounds i8, ptr addrspace(1) %p, i64 12
  %v = load atomic i32, ptr addrspace(1) %sm unordered, align 4
  ret i32 %v
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define i32 @test_box_case_c
; CHECK-NOT: jeandle.new_instance
; CHECK-NOT: store atomic
; CHECK-NOT: load atomic
; The unbox collapses to the field PHI synthesized by Case C.
; CHECK: phi i32 [ %x, %{{[A-Za-z0-9_]+}} ], [ %y, %{{[A-Za-z0-9_]+}} ]

!java-method-compilation = !{}
