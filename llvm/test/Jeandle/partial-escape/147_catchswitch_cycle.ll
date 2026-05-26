; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; A callbr-created back-edge that LoopInfo does NOT model. The alloc +
; store at entry survive in IR because the analyzer's defensive sweep
; marks every still-virtual VO at %head ineligible on detecting the
; unvisited back-edge.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @asm_callbr()
declare i32 @__gxx_personality_v0(...)

define void @test_callbr(i32 %n) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %obj = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
              ptr inttoptr (i64 1010 to ptr), i32 16)
           to label %head unwind label %u

head:
  %slot = getelementptr inbounds i8, ptr addrspace(1) %obj, i64 8
  store atomic i32 7, ptr addrspace(1) %slot unordered, align 4
  callbr void asm "", "!i,!i"() to label %fall [label %head, label %exit]

fall:
  br label %exit

exit:
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @test_callbr
; CHECK: jeandle.new_instance({{.*}}i64 1010
; CHECK: store atomic i32 7

!java-method-compilation = !{}
