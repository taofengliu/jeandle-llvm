; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Edge case: a single virtual alloc flows through both arms of a
; branch unchanged. The merge block has phi(%o, %o); per Case B in
; mergeStates, the phi is aliased to the same virtual ObjectID as %o.
; Then `icmp eq %phi, %o` should fold to true (same ID -> eq=true).

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @use(i32)
declare i32 @__gxx_personality_v0(...)

define void @test_icmp_eq_phi_case_b(i1 %c) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %n unwind label %u
n:
  br i1 %c, label %left, label %right
left:
  br label %merge
right:
  br label %merge
merge:
  %phi = phi ptr addrspace(1) [ %o, %left ], [ %o, %right ]
  %eq = icmp eq ptr addrspace(1) %phi, %o
  br i1 %eq, label %same, label %diff
same:
  call void @use(i32 1)
  ret void
diff:
  call void @use(i32 -1)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; *** Missed-optimization: PEA does not currently exploit Case B alias-of-virt
; through a PHI-of-virt at a non-modifying merge to fold a downstream icmp eq.
; What PEA does today: forces materialization of %o at the alloc point (the
; PHI is treated as escaping the virtual), so the icmp does NOT fold, the
; "diff" arm survives, and the alloc remains.
; What an ideal PEA could do: see that all PHI incomings are the same virtual
; ObjectID, alias the PHI to that ObjectID, fold the icmp eq to true, prune
; the diff arm, and eliminate the alloc entirely. Tracked as a follow-up.
;
; CHECK-LABEL: define void @test_icmp_eq_phi_case_b
; The alloc gets materialized (i.e., a new alloc invoke appears).
; CHECK: invoke hotspotcc{{.*}}ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 12345 to ptr), i32 16)
; Both arms remain reachable in current PEA output.
; CHECK-DAG: call void @use(i32 1)
; CHECK-DAG: call void @use(i32 -1)

!java-method-compilation = !{}
