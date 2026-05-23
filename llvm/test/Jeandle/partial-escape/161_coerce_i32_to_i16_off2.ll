; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; PEA / B6: store i32 into a virtual's slot at offset 8, load i16 at byte
; offset 10 (within-slot byte offset 2). Little-endian semantics: emit
; `lshr i32 V, 16` then `trunc i32 to i16` to extract the high half.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)

declare i32 @__gxx_personality_v0(...)

define i16 @test_coerce_i32_to_i16_off2() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %obj = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
         to label %normal unwind label %unwind

normal:
  %slot = getelementptr inbounds i8, ptr addrspace(1) %obj, i64 8
  %hi   = getelementptr inbounds i8, ptr addrspace(1) %obj, i64 10
  store atomic i32 305419896, ptr addrspace(1) %slot unordered, align 4
  %v = load atomic i16, ptr addrspace(1) %hi unordered, align 2
  ret i16 %v

unwind:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define i16 @test_coerce_i32_to_i16_off2()
; CHECK-NOT: jeandle.new_instance
; CHECK-NOT: store
; CHECK-NOT: load
; CHECK: %[[S:.*]] = lshr i32 305419896, 16
; CHECK: %[[T:.*]] = trunc i32 %[[S]] to i16
; CHECK: ret i16 %[[T]]

!java-method-compilation = !{}
