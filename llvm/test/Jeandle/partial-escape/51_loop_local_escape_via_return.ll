; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; PEA: a loop body allocates an object, stores a loop-invariant arg into
; one field, then conditionally returns the alloc on one branch (= escape)
; or continues on the other. Because the field-store source dominates the
; alloc's SafeIP (it's a function arg), materializeAt's dominance check
; passes and a materialization invoke is emitted at the alloc site. The
; return path observes the materialized pointer; the continue path is
; unaffected by the materialization beyond seeing the new alloc value.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare i32 @__gxx_personality_v0(...)

define ptr addrspace(1) @test_loop_escape_return(i32 %n, i32 %x) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %i1, %cont ]
  %c = icmp slt i32 %i, %n
  br i1 %c, label %body, label %exit
body:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %st unwind label %u
st:
  %s = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  store atomic i32 %x, ptr addrspace(1) %s unordered, align 4
  %ce = icmp eq i32 %i, 7
  br i1 %ce, label %ret, label %cont
ret:
  ret ptr addrspace(1) %o
cont:
  %i1 = add i32 %i, 1
  br label %loop
exit:
  ret ptr addrspace(1) null
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; A new_instance invoke is materialized; the loop-invariant store is
; replayed; the return uses the materialized pointer.
; CHECK-LABEL: define ptr addrspace(1) @test_loop_escape_return
; CHECK: %[[MAT:[A-Za-z0-9._]+]] = invoke hotspotcc{{.*}}@jeandle.new_instance
; CHECK: store atomic i32 %x, ptr addrspace(1) %{{.*}}
; CHECK: ret ptr addrspace(1) %[[MAT]]

!java-method-compilation = !{}
