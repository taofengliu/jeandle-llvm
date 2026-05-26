; RUN: opt -S -passes="partial-escape-iterative" -jeandle-pea-iterations=2 %s | FileCheck %s

; Outer fixpoint demonstration. The escape arm (%escape -> sink) is
; guarded by an `icmp ne` of a load from a constant-zero global; the analyzer
; in round 1 cannot see through the load and so visits %escape, where the
; sink call forces materialization. Between rounds InstCombine folds the load
; and the icmp, SimplifyCFG removes the dead branch, ADCE prunes the
; orphaned mat invoke + sink call. Round 2 sees a single-block control flow
; where %o has only loads/stores to fixed offsets — PEA virtualizes %o end
; to end and the function reduces to `ret i32 42`.
;
; With -jeandle-pea-iterations=2 we expect zero jeandle.new_instance left in
; the function. Compare with 282 (default iterations=1) where the alloc
; survives.

@G_zero = private unnamed_addr constant i32 0

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define i32 @test_two_round_elim()
    gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %n unwind label %u
n:
  ; This load+icmp folds to `false` only after InstCombine; PEA in round 1
  ; cannot see through it.
  %v = load i32, ptr @G_zero, align 4
  %c = icmp ne i32 %v, 0
  br i1 %c, label %escape, label %fast
escape:
  call void @sink(ptr addrspace(1) %o)
  br label %fast
fast:
  %slot = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  store atomic i32 42, ptr addrspace(1) %slot unordered, align 4
  %val = load atomic i32, ptr addrspace(1) %slot unordered, align 4
  ret i32 %val
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define i32 @test_two_round_elim()
; CHECK-NOT: jeandle.new_instance
; CHECK-NOT: call void @sink
; CHECK: ret i32 42

!java-method-compilation = !{}
