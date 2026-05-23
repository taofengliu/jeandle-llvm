; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; A1 — nested loops where the inner-body alloc escapes via a sink. The
; inner loop's fixpoint converges with the obj marked escaped (every
; iteration the materializeAt at the @sink emits a Materialize effect at
; the alloc's SafeIP). The outer-loop fixpoint sees the inner-loop body
; produce a stable BlockExits and converges. The alloc survives in IR.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_nested_overflow(i32 %n, i32 %m) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  br label %outer
outer:
  %i = phi i32 [ 0, %entry ], [ %i1, %outer.next ]
  %oc = icmp slt i32 %i, %n
  br i1 %oc, label %inner, label %exit
inner:
  %j = phi i32 [ 0, %outer ], [ %j1, %inner.cont ]
  %ic = icmp slt i32 %j, %m
  br i1 %ic, label %inner.body, label %outer.next
inner.body:
  %ob = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
              ptr inttoptr (i64 6666 to ptr), i32 16)
          to label %ib.cont unwind label %u
ib.cont:
  call void @sink(ptr addrspace(1) %ob)
  br label %inner.cont
inner.cont:
  %j1 = add i32 %j, 1
  br label %inner
outer.next:
  %i1 = add i32 %i, 1
  br label %outer
exit:
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @test_nested_overflow
; CHECK: invoke {{.*}}@jeandle.new_instance
; CHECK: call void @sink

!java-method-compilation = !{}
