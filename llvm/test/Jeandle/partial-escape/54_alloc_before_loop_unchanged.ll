; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; PEA regression check: an alloc lives in entry and is used inside a
; loop only via a field load (no escape, no field mutation). The preheader
; sees the object as still-virtual at its exit, so materializeBeforeLoops
; force-materializes the alloc at the preheader. The load default-folds to
; zero. The loop-body alloc lift must not regress this case: alloc-before-
; loop is still handled by the preheader sweep, not by tier1's permissive
; policy (which only kicks in for allocs whose parent block is itself in a
; loop).

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @use(i32)
declare i32 @__gxx_personality_v0(...)

define void @test_alloc_before_loop(i32 %n) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %prep unwind label %u
prep:
  br label %loop
loop:
  %i = phi i32 [ 0, %prep ], [ %i1, %body ]
  %c = icmp slt i32 %i, %n
  br i1 %c, label %body, label %exit
body:
  %s = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  %v = load atomic i32, ptr addrspace(1) %s unordered, align 4
  call void @use(i32 %v)
  %i1 = add i32 %i, 1
  br label %loop
exit:
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; The materialization invoke appears (force-materialized at preheader).
; The load folds to its default zero, so @use receives 0.
; CHECK-LABEL: define void @test_alloc_before_loop
; CHECK: invoke hotspotcc{{.*}}@jeandle.new_instance
; CHECK: call void @use(i32 0)

!java-method-compilation = !{}
