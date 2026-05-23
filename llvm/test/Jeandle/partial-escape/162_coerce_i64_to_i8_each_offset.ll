; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; PEA / B6: store i64 into a virtual's slot at offset 8, then perform 8
; separate i8 loads, one at each byte offset 0..7 within the slot. The
; low byte uses `trunc` only; the other seven use `lshr by 8*off` + `trunc`.
; All sub-byte values are summed so each load's result is observed.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)

declare i32 @__gxx_personality_v0(...)

define i8 @test_coerce_i64_to_i8_each_offset() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %obj = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 24)
         to label %normal unwind label %unwind

normal:
  %slot = getelementptr inbounds i8, ptr addrspace(1) %obj, i64 8
  store atomic i64 -81985529216486896, ptr addrspace(1) %slot unordered, align 8
  %p0 = getelementptr inbounds i8, ptr addrspace(1) %obj, i64 8
  %p1 = getelementptr inbounds i8, ptr addrspace(1) %obj, i64 9
  %p2 = getelementptr inbounds i8, ptr addrspace(1) %obj, i64 10
  %p3 = getelementptr inbounds i8, ptr addrspace(1) %obj, i64 11
  %p4 = getelementptr inbounds i8, ptr addrspace(1) %obj, i64 12
  %p5 = getelementptr inbounds i8, ptr addrspace(1) %obj, i64 13
  %p6 = getelementptr inbounds i8, ptr addrspace(1) %obj, i64 14
  %p7 = getelementptr inbounds i8, ptr addrspace(1) %obj, i64 15
  %b0 = load atomic i8, ptr addrspace(1) %p0 unordered, align 1
  %b1 = load atomic i8, ptr addrspace(1) %p1 unordered, align 1
  %b2 = load atomic i8, ptr addrspace(1) %p2 unordered, align 1
  %b3 = load atomic i8, ptr addrspace(1) %p3 unordered, align 1
  %b4 = load atomic i8, ptr addrspace(1) %p4 unordered, align 1
  %b5 = load atomic i8, ptr addrspace(1) %p5 unordered, align 1
  %b6 = load atomic i8, ptr addrspace(1) %p6 unordered, align 1
  %b7 = load atomic i8, ptr addrspace(1) %p7 unordered, align 1
  %s01 = add i8 %b0, %b1
  %s23 = add i8 %b2, %b3
  %s45 = add i8 %b4, %b5
  %s67 = add i8 %b6, %b7
  %s0123 = add i8 %s01, %s23
  %s4567 = add i8 %s45, %s67
  %sum = add i8 %s0123, %s4567
  ret i8 %sum

unwind:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define i8 @test_coerce_i64_to_i8_each_offset()
; CHECK-NOT: jeandle.new_instance
; CHECK-NOT: store
; CHECK-NOT: load atomic
; Low byte: plain trunc, no lshr by 0.
; CHECK: trunc i64 -81985529216486896 to i8
; Other bytes: lshr + trunc at 8, 16, 24, 32, 40, 48, 56.
; CHECK-DAG: lshr i64 -81985529216486896, 8
; CHECK-DAG: lshr i64 -81985529216486896, 16
; CHECK-DAG: lshr i64 -81985529216486896, 24
; CHECK-DAG: lshr i64 -81985529216486896, 32
; CHECK-DAG: lshr i64 -81985529216486896, 40
; CHECK-DAG: lshr i64 -81985529216486896, 48
; CHECK-DAG: lshr i64 -81985529216486896, 56
; CHECK: ret i8

!java-method-compilation = !{}
