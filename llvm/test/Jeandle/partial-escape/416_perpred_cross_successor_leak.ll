; REQUIRES: asserts
; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Per-pred materialization across multiple successors under the reuse-OrigAlloc
; model. `else` is the virtual predecessor of a mixed merge `merge` (then
; escapes o, else keeps it virtual) and itself has TWO successors: `merge` and
; `S`.
;
; Historically this required per-pred materialization of o at `else`'s
; terminator destined for `merge`, and the per-pred flip had to be cloned per
; (PH, target-merge) so it did NOT leak to `S` -- two distinct pea.crit.split
; blocks and two pea.mat invokes were produced off `else`, each merge carrying
; a materialized PHI.
;
; Under reuse-OrigAlloc the original allocation %o dominates BOTH successors
; of `else`, so it is the single SSA value everywhere: no per-pred
; materialization fires, no critical edge is split, and `else`'s original
; two-successor branch is retained verbatim. The tracked field store is
; replayed onto %o on the escaping arm (then), and both escapes (then, S)
; consume %o directly. No %pea.mat, no materialized-object PHI.

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

; CHECK-LABEL: define void @repro
; The original allocation invoke is retained.
; CHECK: invoke hotspotcc{{.*}}@jeandle.new_instance
; No materialization invokes, no critical-edge splits.
; CHECK-NOT: pea.mat = invoke
; CHECK-NOT: pea.crit.split
; The then arm replays the tracked field store onto OrigAlloc %o and escapes.
; CHECK: getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
; CHECK: call void @sink(ptr addrspace(1) %o)
; `else` retains its original two-successor branch (no split off else).
; CHECK: else:
; CHECK-NEXT: br i1 %c2, label %merge, label %S
; The cross-successor escape (S) consumes OrigAlloc %o directly.
; CHECK: call void @sink(ptr addrspace(1) %o)
; CHECK: ret void

!java-method-compilation = !{}
