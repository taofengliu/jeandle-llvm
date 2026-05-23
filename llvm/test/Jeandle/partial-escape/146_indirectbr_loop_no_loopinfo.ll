; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; R9.R7: an indirectbr-created back-edge that LoopInfo does NOT recognise
; as a loop. The block %head has a pred from %indirect that is later in
; RPO than %head (the indirectbr to %head); LoopInfo does not see it as
; a loop. The defensive sweep in Analyzer::run bails every VO virtual at
; entry to %head, so the alloc + stores survive in IR.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare i32 @__gxx_personality_v0(...)

@labels = external addrspace(1) global [2 x ptr]

define void @test_indirectbr(i32 %n, ptr %target) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %obj = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
              ptr inttoptr (i64 9999 to ptr), i32 16)
           to label %head unwind label %u

head:
  %slot = getelementptr inbounds i8, ptr addrspace(1) %obj, i64 8
  store atomic i32 7, ptr addrspace(1) %slot unordered, align 4
  br label %indirect

indirect:
  ; Back-edge to %head via indirectbr — LoopInfo does not model this as
  ; a loop, so the analyzer's RPO walk reaches %head with one unvisited
  ; pred (%indirect, later in RPO).
  indirectbr ptr %target, [label %head, label %exit]

exit:
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @test_indirectbr
; CHECK: jeandle.new_instance({{.*}}i64 9999
; CHECK: store atomic i32 7

!java-method-compilation = !{}
