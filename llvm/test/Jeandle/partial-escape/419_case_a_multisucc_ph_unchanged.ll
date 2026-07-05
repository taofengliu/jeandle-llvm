; REQUIRES: asserts
; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Case-A regression guard: an LLVM pointer PHI at `merge` mixes a virtual
; incoming (o from `else`, which has TWO successors) with a non-virtual
; incoming (null from `then`). processBlockPhis Case-A materializes o at
; `else`'s terminator (SkipGlobalRAUW=false, IsPerPred=false). The mat is
; placed at PH end (NewInv dominates all successors — Graal-aligned), NOT on a
; critical-edge split. The pre-pass must NOT fire (IsPerPred=false), so there
; is NO `pea.crit.split` block, and the materialize invoke's normal-dest
; retains the original multi-successor branch (-> merge and S).

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @repro(i1 %c, i1 %c2)
    gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %n unwind label %u
n:
  br i1 %c, label %then, label %else
then:
  br label %merge
else:
  ; o is virtual. TWO successors: merge and S.
  br i1 %c2, label %merge, label %S
merge:
  ; LLVM PHI mixing virtual (o from else) and non-virtual (null from then)
  ; -> Case A: materialize o at else's terminator (PH end, not a split edge).
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
; One materialize invoke at `else` (Case-A placement, PH end).
; CHECK: pea.mat{{[0-9]*}} = invoke hotspotcc {{.*}}@jeandle.new_instance
; The invoke's normal-dest retains the original multi-successor branch
; (br c2 -> merge and S), proving Case-A placement (not a crit-split edge).
; CHECK: to label %mat.cont
; CHECK: mat.cont:
; CHECK-NEXT: br i1 %c2, label %merge, label %S
; No critical-edge split block for Case A.
; CHECK-NOT: pea.crit.split
; The merge PHI (kept as the original %p) has its else-incoming rewritten to
; the Case-A NewInv (pea.mat) and routed through mat.cont (a real pred).
; CHECK: merge:
; CHECK-NEXT: %p = phi ptr addrspace(1) [ null, %then ], [ %pea.mat, %mat.cont ]
; CHECK: call void @sink
; CHECK: ret void
