; REQUIRES: asserts
; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; PH (`else`) has TWO successors: `merge` (mixed -> per-pred mat at else for
; merge) and `D` (single-pred, o stays virtual, no escape). The per-pred flip
; must NOT leak to `D`: `D` sees o as virtual on the else->D edge (clone not
; shared), so no per-pred mat fires on else->D and no OOM-unwind lives on that
; edge. Only else->merge is split and carries a pea.mat.

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
  %slot = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  store atomic i32 %v, ptr addrspace(1) %slot unordered, align 4
  call void @sink(ptr addrspace(1) %o)
  br label %merge
else:
  ; o is virtual. TWO successors: merge and D.
  br i1 %c2, label %merge, label %D
merge:
  ; Mixed merge of then (materialized) and else (virtual) -> per-pred mat at
  ; else for merge.
  br label %exit
D:
  ; Single-pred (else). o stays virtual here, never escapes. No materialize.
  ret void
exit:
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

!java-method-compilation = !{}

; Exactly ONE critical-edge split off `else` (else->merge), and exactly ONE
; per-pred materialize invoke on that split (plus the then-arm escape-point
; mat). The else->D edge must NOT carry a pea.mat, and `D` has no invoke.
; CHECK: define void @repro
; CHECK: pea.crit.split:
; CHECK: pea.mat{{[0-9]*}} = invoke hotspotcc {{.*}}@jeandle.new_instance
; No SECOND critical-edge split (a second split would be named pea.crit.split1).
; CHECK-NOT: pea.crit.split{{[0-9]+}}
; `D` is single-pred from `else` (no split), no invoke, just ret.
; CHECK: D:
; CHECK-NOT: invoke
; CHECK: ret void
