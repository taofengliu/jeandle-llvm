; REQUIRES: asserts
; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; §2.1 scenario: PH (`else`) has TWO successor merges, BOTH doing per-pred mat
; of the same object (each merge is mixed: `split` arm materialized via sink,
; `else` arm virtual). The fix must produce TWO distinct split edges off `else`
; (one per target merge) with their own NewInv, and each merge's PHI routes
; through its own edge. Before the fix, PHRename/BlockRename were keyed by PH
; alone, so the second split overwrote the first and one merge's PHI incoming
; was mis-routed to a non-predecessor.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @repro(i1 %c, i1 %c2, i1 %c3, i32 %v)
    gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %n unwind label %u
n:
  br i1 %c, label %then1, label %else
then1:
  call void @sink(ptr addrspace(1) %o)
  br label %split
split:
  br i1 %c3, label %merge1, label %merge2
else:
  ; o is virtual. TWO successors: merge1 and merge2, both mixed merges.
  br i1 %c2, label %merge1, label %merge2
merge1:
  ; preds: split (materialized), else (virtual) -> per-pred mat at else for merge1.
  call void @sink(ptr addrspace(1) %o)
  ret void
merge2:
  ; preds: split (materialized), else (virtual) -> per-pred mat at else for merge2.
  call void @sink(ptr addrspace(1) %o)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

!java-method-compilation = !{}

; Two distinct critical-edge split blocks off `else` (one per target merge).
; CHECK: pea.crit.split
; CHECK: pea.crit.split
; Two distinct per-pred materialize invokes at `else`.
; CHECK: pea.mat{{[0-9]*}} = invoke hotspotcc {{.*}}@jeandle.new_instance
; CHECK: pea.mat{{[0-9]*}} = invoke hotspotcc {{.*}}@jeandle.new_instance
; Both merges carry a materialized PHI and end in sink + ret.
; CHECK: merge1:
; CHECK-NEXT: %pea.materialized.phi{{[0-9]*}} = phi ptr addrspace(1)
; CHECK: call void @sink(ptr addrspace(1) %pea.materialized.phi
; CHECK: merge2:
; CHECK-NEXT: %pea.materialized.phi{{[0-9]*}} = phi ptr addrspace(1)
; CHECK: call void @sink(ptr addrspace(1) %pea.materialized.phi
; CHECK: ret void
