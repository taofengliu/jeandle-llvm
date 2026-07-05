; REQUIRES: asserts
; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Issue-1 regression guard: `merge` is a mixed merge for o (then arm materialized
; via sink, else arm virtual) -> per-pred materialize of o at `else` for `merge`
; (IsPerPred, on the else->merge split edge). `merge` ALSO has an explicit LLVM
; pointer PHI `%p` mixing null (then) and o (else) -> processBlockPhis Case A
; would, WITHOUT the per-merge clone threading, see o as STILL VIRTUAL in the
; shared BlockExits[else] (the per-pred mat flipped only the clone) and re-fire
; a SECOND Case-A materialize at `else`'s terminator end (PH end) for the same
; (else, merge, o). That duplicates the invoke and exposes the OOM on every
; else->* edge. With the clone fix, processBlockPhis reads the clone (flipped by
; the per-pred mat) -> sees o materialized -> does NOT re-fire Case A. Assert:
; exactly ONE materialize invoke at `else` for `merge` (the per-pred split), and
; a `pea.crit.split` block (NOT a Case-A PH-end placement).

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

!java-method-compilation = !{}

; CHECK: define void @repro
; Exactly ONE critical-edge split off `else` (the per-pred mat for merge).
; CHECK: pea.crit.split:
; CHECK: pea.mat{{[0-9]*}} = invoke hotspotcc {{.*}}@jeandle.new_instance
; The merge PHI's else-incoming is the per-pred NewInv routed through the split.
; CHECK: merge:
; CHECK: %p = phi ptr addrspace(1) [ null, %{{.*}} ], [ %pea.mat, %mat.cont ]
; CHECK: call void @sink
; No SECOND split (would be pea.crit.split1) and no Case-A mat at `else` end
; (a Case-A placement would put the invoke directly in `else`, not a split).
; CHECK-NOT: pea.crit.split{{[0-9]+}}
; CHECK: ret void
