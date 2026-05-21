; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s
;
; Regression test: the analyzer picks SafeIP =
; computeMaterializationPoint(alloc) = first non-PHI/dbg of the alloc's
; normal-dest block, which here is the array_length call. The array_length
; call also gets a ReplaceLoad effect (folds to constant 4) with a lower
; SeqNo than the later Materialize effect emitted for the @sink escape.
; Pass 1 processes effects in SeqNo order within a block, so the fold
; runs first and erases the SafeIP instruction. applyMaterialize must
; detect the dangling InsertBefore (now a nulled WeakTrackingVH) and
; recover by recomputing a fresh safe IP at the head of the alloc's
; normal-dest block.

; Edge case: a virtual array of length 4 has its array_length read
; before the array escapes. Two scenarios are exercised:
;
;   * test_length_then_eliminate: array_length is read, never escapes.
;     The length call folds to 4, and the alloc is eliminated entirely.
;
;   * test_length_then_escape: array_length is read first (folds to 4),
;     THEN the array escapes via @sink. The length call still folds, but
;     the alloc must be materialized at the escape point.

declare hotspotcc ptr addrspace(1) @jeandle.newarray(ptr, i32)
declare hotspotcc i32 @jeandle.array_length(ptr addrspace(1) readonly)
declare void @use(i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_length_then_eliminate() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %arr = invoke hotspotcc ptr addrspace(1) @jeandle.newarray(
            ptr inttoptr (i64 12345 to ptr), i32 4)
         to label %n unwind label %u
n:
  %len = call hotspotcc i32 @jeandle.array_length(ptr addrspace(1) %arr)
  call void @use(i32 %len)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @test_length_then_eliminate
; CHECK-NOT: jeandle.newarray
; CHECK-NOT: jeandle.array_length
; CHECK: call void @use(i32 4)

define void @test_length_then_escape() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %arr = invoke hotspotcc ptr addrspace(1) @jeandle.newarray(
            ptr inttoptr (i64 12345 to ptr), i32 4)
         to label %n unwind label %u
n:
  %len = call hotspotcc i32 @jeandle.array_length(ptr addrspace(1) %arr)
  call void @use(i32 %len)
  call void @sink(ptr addrspace(1) %arr)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; The length call folds to 4 (independent of the later escape). The array
; itself is materialized at the alloc-site SafeIP (i.e. hoisted to the head
; of the alloc's normal-dest block, which is the recovery target after the
; original SafeIP instruction was folded away by ReplaceLoad).
; CHECK-LABEL: define void @test_length_then_escape
; CHECK-NOT: jeandle.array_length
; CHECK: invoke hotspotcc{{.*}}ptr addrspace(1) @jeandle.newarray
; CHECK: call void @use(i32 4)
; CHECK: call void @sink

!java-method-compilation = !{}
