; REQUIRES: asserts
; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Per-pred + Case-A dedup regression guard under the reuse-OrigAlloc model.
; `merge` is a mixed merge for o (then arm materialized via sink, else arm
; virtual) and ALSO has an explicit LLVM pointer PHI `%p` mixing null (then)
; and o (else) -- a Case-A candidate.
;
; Historically the per-pred materialize at `else` for `merge` (on the
; else->merge split edge) had to win over Case-A: without per-merge clone
; threading, processBlockPhis would re-fire a SECOND Case-A materialize at
; `else`'s terminator end for the same (else, merge, o), duplicating the
; invoke. The test asserted exactly ONE materialize invoke (the per-pred
; split) and no Case-A PH-end placement.
;
; Under reuse-OrigAlloc there is NO materialize at all -- the original %o is
; the single value -- so the dedup concern is trivially satisfied: exactly
; one allocation invoke (the retained original), no pea.mat, no split. The
; merge PHI's else-incoming stays OrigAlloc %o directly.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @repro(i1 %c, i1 %c2, i32 %v)
    gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %n unwind label %u
n:
  br i1 %c, label %then, label %else
then:
  ; Escape o -> materialized on this arm.
  %slot = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  store atomic i32 %v, ptr addrspace(1) %slot unordered, align 4
  call void @sink(ptr addrspace(1) %o)
  br label %merge
else:
  ; o is virtual. TWO successors: merge and S.
  br i1 %c2, label %merge, label %S
merge:
  ; Mixed merge for o (per-pred mat at else) AND an LLVM PHI mixing null + o
  ; (Case A candidate). The per-pred mat must win; Case A must NOT re-fire.
  %p = phi ptr addrspace(1) [ null, %then ], [ %o, %else ]
  call void @sink(ptr addrspace(1) %p)
  ret void
S:
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @repro
; Exactly one allocation invoke (the original, retained) -- no duplication.
; CHECK: invoke hotspotcc{{.*}}@jeandle.new_instance
; CHECK-NOT: invoke hotspotcc{{.*}}@jeandle.new_instance
; The then arm replays the tracked field store onto OrigAlloc %o and escapes.
; CHECK: getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
; CHECK: call void @sink(ptr addrspace(1) %o)
; `else` retains its original two-successor branch (no per-pred split, no
; Case-A mat at PH end).
; CHECK: else:
; CHECK-NEXT: br i1 %c2, label %merge, label %S
; The merge PHI's else-incoming is OrigAlloc %o directly.
; CHECK: merge:
; CHECK-NEXT: %p = phi ptr addrspace(1) [ null, %then ], [ %o, %else ]
; CHECK: call void @sink(ptr addrspace(1) %p)
; CHECK: ret void

!java-method-compilation = !{}
