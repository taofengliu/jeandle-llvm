; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; A6 — alloc INSIDE the loop body, escape AFTER the loop exit. Before A1,
; tier1Allocate refused to virtualize loop-body allocs so the original
; invoke survived; after A1 the body alloc is tracked through the back-
; edge, and the post-loop @sink call materializes the object at the
; alloc's SafeIP (still inside the body). Function-wide RAUW on the
; original alloc means the post-loop use sees the materialized pointer.
;
; Pre-LCSSA, the original IR uses %o directly in the exit block: the body
; block dominates the exit (single-pred exit from body). After RAUW, the
; exit's use snaps onto the materialized invoke that also lives in the
; body block — dominance is preserved by construction.
;
; This exercises the "alloc-in-loop + escape-after-loop" path that the
; pre-A1 force-materialize-at-preheader behaviour could not reach.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_a6_alloc_in_loop_use_after(i32 %n, i32 %x) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %i1, %cont ]
  %c = icmp slt i32 %i, %n
  br i1 %c, label %body, label %exit_null
body:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %st unwind label %u
st:
  %s = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  store atomic i32 %x, ptr addrspace(1) %s unordered, align 4
  %ce = icmp eq i32 %i, 7
  br i1 %ce, label %exit_use, label %cont
cont:
  %i1 = add i32 %i, 1
  br label %loop
exit_use:
  ; Post-loop use of the body-local alloc. After A1 + RAUW, %o refers to
  ; the materialized invoke produced inside the body block.
  call void @sink(ptr addrspace(1) %o)
  ret void
exit_null:
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; The body materializes the alloc at its SafeIP; the field store is
; replayed; the post-loop @sink call receives the materialized pointer
; via the function-wide RAUW. Exactly one new_instance invoke survives,
; and there is no separate preheader force-materialize.
; CHECK-LABEL: define void @test_a6_alloc_in_loop_use_after
; CHECK: %[[MAT:[A-Za-z0-9._]+]] = invoke hotspotcc{{.*}}@jeandle.new_instance
; CHECK: store atomic i32 %x, ptr addrspace(1) %{{.*}}
; CHECK: call void @sink(ptr addrspace(1) %[[MAT]])

!java-method-compilation = !{}
