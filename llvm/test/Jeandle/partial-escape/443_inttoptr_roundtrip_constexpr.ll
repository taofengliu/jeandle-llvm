; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; ConstantExpr inttoptr/ptrtoint round-trip wrapper-stripping (review §3.1).
;
; getIntToPtrRoundTripInner and the resolveVirtualRefImpl case-7 gate used
; Instruction-only casts (`IntToPtrInst`/`PtrToIntInst`), so a ConstantExpr
; round-trip was opaque to BOTH identity and offset resolution (symmetric, so
; sound but a missed opt). The shared walker stripPointerCastsAndOffsets
; already used `*Operator` for GEP/BitCast/AddrSpaceCast; only the
; inttoptr/ptrtoint branch lagged. After the fix both paths use Operator-form
; casts so a ConstantExpr round-trip is peeled the same way an
; Instruction-form round-trip is, keeping Trap-4 identity/offset symmetry.
;
; Honest scope note (req-#6 robustness, NOT virtualization-observable): a
; ConstantExpr round-trip wraps only CONSTANT pointers (a runtime virtual
; pointer is non-constant), so this construct cannot by itself exercise a
; virtual reference through the round-trip — the pass behavior is identical
; with and without the fix for any constructible IR here. The value of this
; test is as a non-crash / symmetry guard: the `*Operator` path runs, no
; miscompile or verifier failure is produced, and the offset path's
; wrapper-stripping stays symmetric with the identity path. See the sibling
; Instruction-form test 407 for the virtualization-observable case.

@g = global i32 0

declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_inttoptr_roundtrip_constexpr() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  ; ConstantExpr round-trip of a constant pointer, wrapped in a GEP exercised
  ; by PEA's offset resolver. The inttoptr(ptrtoint(@g)) round-trip is a
  ; ConstantExpr, not an Instruction, so the old `*Inst` casts missed it.
  %p = getelementptr i8, ptr addrspace(1) inttoptr (i64 ptrtoint (ptr @g to i64) to ptr addrspace(1)), i64 8
  store i32 111, ptr addrspace(1) %p
  call void @sink(ptr addrspace(1) %p)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @test_inttoptr_roundtrip_constexpr
; The pass completes cleanly; the store through the round-trip-wrapped pointer
; survives (no virtual is involved, so nothing is eliminated).
; CHECK: store i32 111
; CHECK: call void @sink

!java-method-compilation = !{}
