; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; PEA: the allocation lives inside a loop body and escapes via @sink on
; every iteration. processAllocation virtualizes loop-body allocs; the @sink
; call forces materialization at the alloc's SafeIP (still inside the loop).
; The IR therefore still contains a
; jeandle.new_instance invoke per iteration — just rewritten through the
; analyzer's materialization machinery instead of left untouched.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_loop_alloc(i32 %n) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %i1, %use ]
  %c = icmp slt i32 %i, %n
  br i1 %c, label %body, label %exit
body:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %use unwind label %u
use:
  call void @sink(ptr addrspace(1) %o)
  %i1 = add i32 %i, 1
  br label %loop
exit:
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; Loop-body alloc is preserved as an invoke.
; CHECK-LABEL: define void @test_loop_alloc
; CHECK: invoke {{.*}}@jeandle.new_instance

!java-method-compilation = !{}
