; REQUIRES: asserts
; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; per-pred materialize must NOT leak a per-block placeholder across successors.
;
; `else` is the virtual predecessor of a mixed merge `merge` (then escapes o,
; else keeps it virtual) -> per-pred materialize of o at `else`'s terminator
; destined for `merge`. `else` has TWO successors: `merge` and `S`. Before the
; fix, the per-pred flip mutated the shared BlockExits[else], so `S` inherited
; the placeholder (whose real NewInv only dominates the else->merge split edge)
; and built a CreatePHI naming else; the transform's pre-pass split both
; else->merge and else->S but keyed PHRename/BlockRename by PH alone, so the
; second split overwrote the first -> CreatePHI incoming mis-routed to a
; non-predecessor -> verifier abort.
;
; With the fix (per-merge pred-state clone + (PH, target-merge) keying), `S`
; sees o as virtual on the else->S edge (clone not shared), triggers its OWN
; per-pred mat on the else->S split edge, and the two split edges route
; independently. The result is well-formed: two `pea.crit.split` blocks (one
; per target merge), two distinct `pea.mat` invokes at `else`, and both merge
; PHIs have correct predecessors.

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
  ; o is virtual here. TWO successors: merge and S.
  br i1 %c2, label %merge, label %S
merge:
  ; Mixed merge of then (materialized) and else (virtual) -> per-pred
  ; materialize of o at else's terminator, destined for `merge`.
  br label %S
S:
  ; Merge of else (o virtual -> own per-pred mat on else->S) and merge
  ; (o = real materialized PHI) -> per-pred mat on else->S + CreatePHI.
  call void @sink(ptr addrspace(1) %o)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

!java-method-compilation = !{}

; CHECK: define void @repro
; Two distinct critical-edge split blocks off `else` (one per target merge).
; CHECK: pea.crit.split
; CHECK: pea.crit.split
; Two distinct per-pred materialize invokes at `else` (one per split edge).
; CHECK: pea.mat{{[0-9]*}} = invoke hotspotcc {{.*}}@jeandle.new_instance
; CHECK: pea.mat{{[0-9]*}} = invoke hotspotcc {{.*}}@jeandle.new_instance
; The `merge` block is a predecessor of `S` and carries a materialized PHI
; whose incomings are the then-arm NewInv and the else->merge split NewInv.
; CHECK: merge:
; CHECK-NEXT: %pea.materialized.phi{{[0-9]*}} = phi ptr addrspace(1)
; CHECK-SAME: %pea.mat
; CHECK-SAME: %pea.mat
; `S` merges the else->S split NewInv with `merge`'s materialized PHI; both
; incoming blocks are real predecessors of S.
; CHECK: S:
; CHECK-NEXT: %pea.materialized.phi{{[0-9]*}} = phi ptr addrspace(1)
; CHECK-SAME: %pea.materialized.phi
; CHECK-SAME: %pea.mat
; CHECK: call void @sink(ptr addrspace(1) %pea.materialized.phi
; CHECK: ret void
