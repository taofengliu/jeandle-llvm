; RUN: opt -S -passes="partial-escape-iterative" -jeandle-pea-iterations=2 %s | FileCheck %s

; Natural-path re-foldable materialize. Round 1 of PEA materializes %o
; at the escape arm via applyMaterialize, which emits a fresh
; `jeandle.new_instance` invoke ("pea.mat") followed by separate `store`
; instructions for each tracked field. After canonicalization between rounds
; (InstCombine folds the load+icmp, SimplifyCFG drops the now-dead escape
; arm, ADCE/the dead-code sweep cleans up), the analyzer's RPO walk in
; round 2 sees the freshly materialized invoke as a brand-new allocation
; site — processAllocation recognises it, the residual field stores are picked
; up naturally as virtual-field writes, and the alloc is re-virtualized
; and eliminated.
;
; This is the "without metadata" path: no AllocatedObjectNode-style
; back-pointer is needed because the IR shape the materializer emits is
; already in the form PEA can re-process.
;
; What differs from 280: we write TWO fields, then read them back on the
; fast path. The two stores survive round 1 (since the alloc was
; materialized) and both are re-folded into the virtual state in round 2.

@G_zero = private unnamed_addr constant i32 0

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define i64 @test_b8_natural_refold()
    gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 24)
       to label %n unwind label %u
n:
  %v = load i32, ptr @G_zero, align 4
  %c = icmp ne i32 %v, 0
  br i1 %c, label %escape, label %fast
escape:
  call void @sink(ptr addrspace(1) %o)
  br label %fast
fast:
  %slot1 = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  %slot2 = getelementptr inbounds i8, ptr addrspace(1) %o, i64 16
  store atomic i32 7, ptr addrspace(1) %slot1 unordered, align 4
  store atomic i32 35, ptr addrspace(1) %slot2 unordered, align 4
  %v1 = load atomic i32, ptr addrspace(1) %slot1 unordered, align 4
  %v2 = load atomic i32, ptr addrspace(1) %slot2 unordered, align 4
  %sum32 = add i32 %v1, %v2
  %sum = sext i32 %sum32 to i64
  ret i64 %sum
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define i64 @test_b8_natural_refold()
; Round 2 folds the loads to the constants and the dead escape branch is
; removed (no sink). With escape-point placement a dead materialize may survive
; (feeding the fast path, not re-virtualized by round 2); full elimination is a
; future escape-point + outer-fixpoint refinement.
; CHECK-NOT: call void @sink
; CHECK: ret i64 42

!java-method-compilation = !{}
