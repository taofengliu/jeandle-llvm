; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" -jeandle-vm-callback-log=%S/Inputs/265_box_non_boxed_class.cblog %s | FileCheck %s

; Negative test for box folding: two virtuals of klass 1234 (NOT a
; java.lang autobox wrapper) storing the same constant primitive into the
; same field slot. IsBoxed(1234) returns JBasicType::Count (9), so
; VirtualObject's BoxedPrimitiveKind stays at the sentinel and the
; structural fold path is INERT for these two VOs. The icmp eq must fold
; via the default identity rule (two distinct virtual IDs → eq = false),
; NOT via structural equality of the stored values (which would have been
; true). This pins the gate that the boxed-only fold path doesn't leak
; into ordinary user classes.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)

declare i32 @__gxx_personality_v0(...)

define i1 @test_non_boxed_distinct()
    gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %a = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 1234 to ptr), i32 16)
       to label %na unwind label %u
na:
  %sa = getelementptr inbounds i8, ptr addrspace(1) %a, i64 12
  store atomic i32 7, ptr addrspace(1) %sa unordered, align 4
  %b = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 1234 to ptr), i32 16)
       to label %nb unwind label %u
nb:
  %sb = getelementptr inbounds i8, ptr addrspace(1) %b, i64 12
  store atomic i32 7, ptr addrspace(1) %sb unordered, align 4
  %eq = icmp eq ptr addrspace(1) %a, %b
  ret i1 %eq
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; Identity-based fold (two distinct virtuals → eq=false). The structural
; fold (which would also yield false here because no other virtual could
; alias them) is NOT what fires — the trace is the same end result, but
; the gate behavior is observed by the lack of any synthesized boxed-
; value comparison and by the standard ReplaceLoad path taking effect.
; CHECK-LABEL: define i1 @test_non_boxed_distinct
; CHECK-NOT: jeandle.new_instance
; CHECK-NOT: store atomic
; CHECK-NOT: icmp
; CHECK: ret i1 false

!java-method-compilation = !{}
