; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; A6 — alloc BEFORE the loop, materialized INSIDE the loop body (via
; @sink), then the same pointer is used AFTER the loop (the @ret_use call
; in the exit block). The materialized invoke is hoisted to the alloc's
; SafeIP (which is in entry's normal-dest = %prep), so NewInv dominates
; both the loop body and the post-loop use. applyMaterialize RAUWs the
; original alloc function-wide, so the post-loop call's argument
; automatically snaps to the materialized pointer — no explicit exit-block
; PHI synthesis is needed.
;
; A6 verification: the post-loop use sees the materialized pointer.

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
