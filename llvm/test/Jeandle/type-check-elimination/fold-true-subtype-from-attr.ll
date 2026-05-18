; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/fold-true-subtype-from-attr.cblog %s 2>&1 | FileCheck %s

; Test: Object type from parameter attribute is a subtype of the check target.
; param has "java-klass"="11" (ArrayList), checking instanceof 1 (Object).
; Every class is a subtype of Object. IsSubtype(11, 1) = true => fold to true.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @test(ptr addrspace(1) nonnull "java-klass"="11" %obj) gc "hotspotgc" {
entry:
  %result = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 1 to ptr),
    ptr addrspace(1) nonnull %obj)
  ret i1 %result
}

; CHECK: ret i1 true

!java-method-compilation = !{}
