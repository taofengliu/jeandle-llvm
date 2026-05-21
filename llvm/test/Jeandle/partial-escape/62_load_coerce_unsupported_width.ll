; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; PEA: different bit widths are not yet supported (would require
; trunc / lshr+trunc with offset-within-slot tracking). Storing i64 and
; loading i32 should bail to ineligible, leaving the original IR shape
; (alloc, store, load) intact.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)

declare i32 @__gxx_personality_v0(...)

define i32 @test_coerce_bail_widths() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %obj = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 24)
         to label %normal unwind label %unwind

normal:
  %slot = getelementptr inbounds i8, ptr addrspace(1) %obj, i64 8
  store atomic i64 1234567890, ptr addrspace(1) %slot unordered, align 8
  %v = load atomic i32, ptr addrspace(1) %slot unordered, align 4
  ret i32 %v

unwind:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; The base must remain ineligible: the original allocation, store, and load
; survive in IR. No bitcast was synthesized.
; CHECK-LABEL: define i32 @test_coerce_bail_widths()
; CHECK: jeandle.new_instance
; CHECK: store atomic i64
; CHECK: load atomic i32
; CHECK-NOT: pea.coerce
; CHECK: ret i32

!java-method-compilation = !{}
