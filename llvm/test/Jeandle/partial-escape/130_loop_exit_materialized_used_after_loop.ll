; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Alloc BEFORE the loop, materialized INSIDE the loop body (via @sink that
; flows to the latch), then the same pointer is used AFTER the loop. Loop-body
; partial escape materializes the object exactly once at the preheader end
; (Graal materializedValuePhi at the loop header, which is trivial here and
; folds), and both the in-loop escape and the post-loop use resolve to that
; single materialized pointer. (Previously the hoist + post-body merge
; accidentally produced TWO allocations here.)
;
; Verifies the post-loop use sees the single materialized pointer.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare void @ret_use(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_a6_mat_in_loop_use_after(i32 %n, i32 %x) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %prep unwind label %u
prep:
  %s = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  store atomic i32 %x, ptr addrspace(1) %s unordered, align 4
  br label %loop
loop:
  %i = phi i32 [ 0, %prep ], [ %i1, %body ]
  %c = icmp slt i32 %i, %n
  br i1 %c, label %body, label %exit
body:
  call void @sink(ptr addrspace(1) %o)
  %i1 = add i32 %i, 1
  br label %loop
exit:
  call void @ret_use(ptr addrspace(1) %o)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; The alloc is materialized exactly once (at the alloc's SafeIP, before the
; loop); both the in-loop @sink and the post-loop @ret_use receive the
; same materialized pointer.
; CHECK-LABEL: define void @test_a6_mat_in_loop_use_after
; CHECK: %[[MAT:[A-Za-z0-9._]+]] = invoke hotspotcc{{.*}}@jeandle.new_instance
; CHECK: store atomic i32 %x, ptr addrspace(1) %{{.*}}
; CHECK: call void @sink(ptr addrspace(1) %[[MAT]])
; CHECK: call void @ret_use(ptr addrspace(1) %[[MAT]])
; CHECK-NOT: jeandle.new_instance

!java-method-compilation = !{}
