; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; A synthetic VO (created by Case C at %M1) flowing into a downstream
; mixed PHI Case A. Without the synthetic-VO bail in
; materializeAtPredFromExitInfo, the Case A path would emit a
; Materialize effect using the synthetic's borrowed OrigAlloc (one of
; the per-pred source allocs), then RAUW that source alloc — which is
; the incoming value of the Case-C merge PHI %p in %M1. The RAUW
; redirected %p's incoming onto the new invoke in %X (downstream of M1),
; breaking SSA dominance:
;
;   "Instruction does not dominate all uses!"
;   %pea.mat = invoke ... @jeandle.new_instance ... to label %mat.cont ...
;   %p = phi ptr addrspace(1) [ %pea.mat, %An ], [ poison, %Bn ]
;
; With the bail in place, materializeAtPredFromExitInfo on a synthetic VO
; marks both the synthetic and every per-pred source ineligible, so the
; original IR (allocs, PHI %p, sink call) survives unchanged.
;
; The general "alloc doesn't dominate merge" case in mergeStates' true-mixed
; branch is unreachable for non-synthetic VOs by SSA dominance: ID can only
; be tracked at every pred of a merge if its OrigAlloc reaches every pred via
; SSA, which by dominance requires OrigAlloc to dominate the merge. A
; debug-only assertion at that branch catches any future regression.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define ptr addrspace(1) @test_a3_synthetic_mixed(
        i1 %c0, i1 %c1, i1 %c2, i1 %c3, ptr addrspace(1) %arg)
    gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  br i1 %c0, label %A, label %B
A:
  %oA = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %An unwind label %u
An:
  br label %M1
B:
  %oB = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %Bn unwind label %u
Bn:
  br label %M1
M1:
  ; Case C synthesizes a single VO aliased to %p.
  %p = phi ptr addrspace(1) [ %oA, %An ], [ %oB, %Bn ]
  br i1 %c1, label %X, label %Z
X:
  br i1 %c2, label %M2, label %N
Z:
  br i1 %c3, label %M2, label %W
M2:
  ; Mixed PHI Case A: %p (synthetic VO alias) on the X edge, %arg
  ; (non-virtual) on the Z edge. Drives materializeAtPredFromExitInfo
  ; on the synthetic at %X — bails on the synthetic VO.
  %q = phi ptr addrspace(1) [ %p, %X ], [ %arg, %Z ]
  call void @sink(ptr addrspace(1) %q)
  ret ptr addrspace(1) %arg
W:
  br label %N
N:
  ret ptr addrspace(1) %arg
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; Expected behavior: PEA bails cleanly; both original allocs and both PHIs
; survive in IR.
; CHECK-LABEL: define ptr addrspace(1) @test_a3_synthetic_mixed
; CHECK: %oA = invoke hotspotcc{{.*}}@jeandle.new_instance
; CHECK: %oB = invoke hotspotcc{{.*}}@jeandle.new_instance
; CHECK: %p = phi ptr addrspace(1) [ %oA, %An ], [ %oB, %Bn ]
; CHECK: %q = phi ptr addrspace(1) [ %p, %X ], [ %arg, %Z ]
; CHECK: call void @sink(ptr addrspace(1) %q)

!java-method-compilation = !{}
