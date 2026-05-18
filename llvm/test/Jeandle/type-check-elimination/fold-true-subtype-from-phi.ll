; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/fold-true-subtype-from-phi.cblog %s 2>&1 | FileCheck %s

; Test: PHI with all incomings being subtypes of the check target.
; Incoming 1: klass 11 (ArrayList), Incoming 2: klass 12 (LinkedList).
; LCA = klass 10 (AbstractList). Checking instanceof klass 1 (Object).
; IsSubtype(10, 1) = true => fold to true.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

@glob1 = external addrspace(1) global ptr addrspace(1)
@glob2 = external addrspace(1) global ptr addrspace(1)

define i1 @test(i1 %cond) gc "hotspotgc" {
entry:
  br i1 %cond, label %left, label %right

left:
  %obj1 = load ptr addrspace(1), ptr addrspace(1) @glob1, !java-klass !0
  br label %merge

right:
  %obj2 = load ptr addrspace(1), ptr addrspace(1) @glob2, !java-klass !1
  br label %merge

merge:
  %obj = phi ptr addrspace(1) [ %obj1, %left ], [ %obj2, %right ]
  %result = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 1 to ptr),
    ptr addrspace(1) nonnull %obj)
  ret i1 %result
}

!0 = !{i64 11}
!1 = !{i64 12}

; CHECK: ret i1 true

!java-method-compilation = !{}
