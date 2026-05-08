; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/instanceof-object.cblog %s 2>&1 | FileCheck %s

; Test: instanceof Object (klass 1) always folds to true for non-null oop.
; The check_instanceof contract guarantees non-null, so IsObjectKlass(1) => true.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @test(ptr addrspace(1) nonnull %obj) gc "hotspotgc" {
entry:
  %result = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 1 to ptr),
    ptr addrspace(1) nonnull %obj)
  ret i1 %result
}

; CHECK: ret i1 true

!java-method-compilation = !{}
