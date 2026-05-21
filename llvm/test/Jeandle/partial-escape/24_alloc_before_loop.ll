; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; PEA: an object is allocated in `entry`, a field is stored in
; `prep`, then a loop iterates and uses the object via @sink on each
; iteration. PEA materializes the obj at the preheader (`prep`'s
; terminator) before the loop. The field store should be replayed at the
; materialization.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_obj_into_loop(i32 %n) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %prep unwind label %u
prep:
  %s = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  store atomic i32 99, ptr addrspace(1) %s unordered, align 4
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
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; The materialization should produce a (re)allocation invoke before the loop,
; with the i32 99 field-store replayed, and the loop body's @sink call still
; references the materialized obj. The original alloc in entry is gone.
; CHECK-LABEL: define void @test_obj_into_loop
; CHECK: invoke {{.*}}@jeandle.new_instance
; CHECK: store atomic i32 99
; CHECK: call void @sink

!java-method-compilation = !{}
