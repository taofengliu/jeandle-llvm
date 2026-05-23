; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; R9.R7: a switch-created back-edge to a non-loop-header block. The
; %head block has %indirect as an RPO-later pred via the switch's
; default case. LoopInfo does not always recognise such constructs as a
; loop, so the analyzer's defensive sweep bails virtuals at %head entry.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare i32 @__gxx_personality_v0(...)

define void @test_switch_backedge(i32 %sel) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %obj = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
              ptr inttoptr (i64 1111 to ptr), i32 16)
           to label %head unwind label %u

head:
  %slot = getelementptr inbounds i8, ptr addrspace(1) %obj, i64 8
  store atomic i32 9, ptr addrspace(1) %slot unordered, align 4
  br label %indirect

indirect:
  switch i32 %sel, label %head [ i32 0, label %exit ]

exit:
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @test_switch_backedge
; The R7 sweep recognises that LoopInfo did not see this as a loop; the
; allocator's defensive ineligibility flip leaves the alloc and the
; store untouched in IR.
; CHECK: jeandle.new_instance({{.*}}i64 1111

!java-method-compilation = !{}
