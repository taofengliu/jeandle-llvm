; REQUIRES: asserts
; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; PH (`else`) has THREE successors via a switch: merge1, merge2 (both mixed ->
; per-pred mat at else) and D (o virtual, no escape). Two critical edges
; (else->merge1, else->merge2) must be split with their own NewInv; the else->D
; edge (D single-pred, not critical) is unchanged and carries no pea.mat.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @repro(i1 %c, i1 %c3, i32 %c2, i32 %v)
    gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %n unwind label %u
n:
  br i1 %c, label %then, label %else
then:
  call void @sink(ptr addrspace(1) %o)
  br label %split
split:
  br i1 %c3, label %merge1, label %merge2
else:
  ; o is virtual. THREE successors via switch: merge1, merge2, D.
  switch i32 %c2, label %D [ i32 0, label %merge1
                              i32 1, label %merge2 ]
merge1:
  ; preds: split (materialized), else (virtual) -> per-pred at else for merge1.
  call void @sink(ptr addrspace(1) %o)
  ret void
merge2:
  ; preds: split (materialized), else (virtual) -> per-pred at else for merge2.
  call void @sink(ptr addrspace(1) %o)
  ret void
D:
  ; Single-pred (else). o stays virtual, no escape.
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

!java-method-compilation = !{}

; CHECK: define void @repro
; Two distinct critical-edge split blocks off `else` (named pea.crit.split and
; pea.crit.split1), each carrying a per-pred materialize invoke.
; CHECK: pea.crit.split{{[0-9]*}}:
; CHECK: pea.mat{{[0-9]*}} = invoke hotspotcc {{.*}}@jeandle.new_instance
; CHECK: pea.crit.split{{[0-9]*}}:
; CHECK: pea.mat{{[0-9]*}} = invoke hotspotcc {{.*}}@jeandle.new_instance
; No THIRD split (D is single-pred, no critical edge).
; CHECK-NOT: pea.crit.split{{[0-9]+}}:
; `D` has no invoke, just ret.
; CHECK: D:
; CHECK-NOT: invoke
; CHECK: ret void
