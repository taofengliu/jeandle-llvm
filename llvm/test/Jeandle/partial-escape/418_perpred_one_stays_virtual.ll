; REQUIRES: asserts
; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Per-pred materialization with a non-target successor under the
; reuse-OrigAlloc model. PH (`else`) has TWO successors: `merge` (mixed) and
; `D` (single-pred, o stays virtual, never escapes).
;
; Historically the per-pred flip had to NOT leak to `D`: `D` saw o as virtual
; on the else->D edge (clone not shared), so no per-pred mat fired there and
; no OOM-unwind lived on that edge; only else->merge was split with a pea.mat.
;
; Under reuse-OrigAlloc the original allocation %o dominates every successor,
; so it is the single value on ALL edges: no per-pred mat fires anywhere, no
; edge is split. `D` still has no invoke (it never escapes), and its `ret` is
; unchanged. The then-arm escape replays the tracked field store onto %o and
; sinks %o.

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

; CHECK-LABEL: define void @repro
; The original allocation invoke is retained.
; CHECK: invoke hotspotcc{{.*}}@jeandle.new_instance
; No materialization invokes and no critical-edge splits anywhere.
; CHECK-NOT: pea.mat = invoke
; CHECK-NOT: pea.crit.split
; The then arm replays the tracked field store onto OrigAlloc %o and escapes.
; CHECK: getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
; CHECK: call void @sink(ptr addrspace(1) %o)
; `else` retains its original two-successor branch (no split off else).
; CHECK: else:
; CHECK-NEXT: br i1 %c2, label %merge, label %D
; `D` is single-pred, o never escapes here: no invoke, just ret.
; CHECK: D:
; CHECK-NOT: invoke
; CHECK: ret void

!java-method-compilation = !{}
