; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/fold-false-non-exact-neither-subtype.cblog %s 2>&1 | FileCheck %s

; Test: Non-exact object type incompatible with target via areKlassesIncompatible.
; obj klass=6 (Animal, non-exact), checking instanceof klass 2 (String).
; Animal is NOT a subtype of String, String is NOT a subtype of Animal,
; Animal is NOT an interface, String is NOT an interface.
; areKlassesIncompatible: IsSubtype(6,2)=false, IsInterface(6)=false,
;   Exact=false => check (!IsSubtype(2,6) && !IsInterface(2))
;   IsSubtype(2,6)=false, IsInterface(2)=false => incompatible => fold to false.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

@glob = external addrspace(1) global ptr addrspace(1)

define i1 @test() gc "hotspotgc" {
entry:
  %obj = load ptr addrspace(1), ptr addrspace(1) @glob, !java-klass !0
  %result = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 2 to ptr),
    ptr addrspace(1) nonnull %obj)
  ret i1 %result
}

!0 = !{i64 6}

; CHECK: ret i1 false

!java-method-compilation = !{}
