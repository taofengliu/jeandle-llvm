; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/instanceof-incompatible.cblog %s 2>&1 | FileCheck %s

; Test: object with exact klass 2 (String, final) is incompatible with klass 3 (Runnable, interface).
; String is not a subtype of Runnable, String is not an interface,
; and since String is exact, areKlassesIncompatible => true, so the check folds to false.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

@glob = external addrspace(1) global ptr addrspace(1)

define i1 @test() gc "hotspotgc" {
entry:
  %obj = load ptr addrspace(1), ptr addrspace(1) @glob, !java-klass !0, !java-klass-exact !1
  %result = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 3 to ptr),
    ptr addrspace(1) nonnull %obj)
  ret i1 %result
}

!0 = !{i64 2}
!1 = !{}

; CHECK: ret i1 false

!java-method-compilation = !{}
