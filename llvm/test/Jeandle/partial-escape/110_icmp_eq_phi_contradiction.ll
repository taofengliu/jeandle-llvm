; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Contradiction rule: a Case-B PHI carries the virtual ObjectID forward
; through both arms, then `icmp eq %phi, %arg` where %arg is a non-null,
; non-virtual pointer must fold to false (distinct identity).
; A virtual is a fresh, just-allocated object — it cannot alias any
; pre-existing reference. eq -> false.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @use(i32)
declare i32 @__gxx_personality_v0(...)

define void @test_icmp_eq_phi_contradiction(i1 %c, ptr addrspace(1) nonnull %arg)
    gc "hotspotgc" personality ptr @__gxx_personality_v0 {
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
  %eq = icmp eq ptr addrspace(1) %phi, %arg
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

; The virtual is fresh — it cannot alias the function-arg pointer %arg.
; The icmp must fold to false, the same arm is dead, and the alloc is
; eliminated (no consumer survives).
; CHECK-LABEL: define void @test_icmp_eq_phi_contradiction
; CHECK-NOT: @jeandle.new_instance
; CHECK-NOT: pea.mat
; CHECK: call void @use(i32 -1)
; CHECK-NOT: call void @use(i32 1)

!java-method-compilation = !{}
