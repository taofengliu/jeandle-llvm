; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/fold-true-same-klass.cblog %s 2>&1 | FileCheck %s

; Test: Object klass equals SuperKlass. IsSubtype(A, A) is always true.
; obj klass=4 (MyRunnable), checking instanceof klass 4 (MyRunnable).
; IsSubtype(4, 4) = true => fold to true.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

@glob = external addrspace(1) global ptr addrspace(1)

define i1 @test() gc "hotspotgc" {
entry:
  %obj = load ptr addrspace(1), ptr addrspace(1) @glob, !java-klass !0
  %result = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 4 to ptr),
    ptr addrspace(1) nonnull %obj)
  ret i1 %result
}

!0 = !{i64 4}

; CHECK: ret i1 true

!java-method-compilation = !{}
