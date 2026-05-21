; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; PEA: jeandle.array_length on a virtual (non-escaping) array
; allocation folds to the array's compile-time constant length.

declare hotspotcc ptr addrspace(1) @jeandle.newarray(ptr, i32)
declare hotspotcc i32 @jeandle.array_length(ptr addrspace(1) readonly)

declare i32 @__gxx_personality_v0(...)

define i32 @test_arraylength() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %arr = invoke hotspotcc ptr addrspace(1) @jeandle.newarray(
            ptr inttoptr (i64 12345 to ptr), i32 7)
         to label %n unwind label %u
n:
  %len = call hotspotcc i32 @jeandle.array_length(ptr addrspace(1) %arr)
  ret i32 %len
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define i32 @test_arraylength
; CHECK-NOT: jeandle.newarray
; CHECK-NOT: jeandle.array_length
; CHECK: ret i32 7

!java-method-compilation = !{}
