; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Nested-synthetic ineligibility cascade. Two sequential Case-C merges build a
; synthetic-source DAG two levels deep:
;
;   M1: %p1 = phi [ %A, %a ], [ %B, %b ]            -> synth S1 over {A, B}
;   M2: %p2 = phi [ %p1, %d ], [ %C, %c ]           -> synth S2 over {S1, C}
;
; S1 is a synthetic VO, and it is itself a per-pred SOURCE of S2 (synthesizeCaseC
; does not reject synthetic sources — %p1 resolves to S1 via the function-wide
; alias map). Then @sink(%p2) escapes S2, so S2 must be dropped to ineligible.
;
; The conservative intent is that EVERY leaf real allocation under the dropped
; synthetic tree survives in IR — here A, B (under S1) AND C (a direct source of
; S2). A single-level cascade marks only S2, S1, and C; A and B stay eligible and
; get eliminated, leaving %p1 = phi [poison, poison] flowing into the escaped
; %p2. The transitive cascade walks S1 -> {A,B} so all three survive with real
; pointers (no poison).

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_casec_nested_cascade(i1 %c1, i1 %c2)
    gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  br i1 %c1, label %a, label %b
a:
  %A = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
          ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %m1 unwind label %u
b:
  %B = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
          ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %m1 unwind label %u
m1:
  %p1 = phi ptr addrspace(1) [ %A, %a ], [ %B, %b ]
  br i1 %c2, label %c, label %d
c:
  %C = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
          ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %m2 unwind label %u
d:
  br label %m2
m2:
  %p2 = phi ptr addrspace(1) [ %p1, %d ], [ %C, %c ]
  call void @sink(ptr addrspace(1) %p2)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @test_casec_nested_cascade
; All three leaf allocations survive — the transitive cascade reached A and B
; through the nested synthetic S1. Exactly three new_instance invokes remain.
; CHECK: invoke hotspotcc{{.*}}@jeandle.new_instance
; CHECK: invoke hotspotcc{{.*}}@jeandle.new_instance
; CHECK: invoke hotspotcc{{.*}}@jeandle.new_instance
; No allocation was eliminated, so no poison flows into the escaped PHI.
; CHECK-NOT: poison
; The escape is preserved.
; CHECK: call void @sink

!java-method-compilation = !{}
