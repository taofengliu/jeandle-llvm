; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/type-from-select.cblog %s 2>&1 | FileCheck %s

; Test: Object type from select instruction. Both operands have types that are
; subtypes of the check target. Select computes type union (LCA).
; True value: klass 11 (ArrayList extends AbstractList)
; False value: klass 12 (LinkedList extends AbstractList)
; LCA = klass 10 (AbstractList). Checking instanceof klass 1 (Object).
; IsSubtype(10, 1) = true => fold to true.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

@glob1 = external addrspace(1) global ptr addrspace(1)
@glob2 = external addrspace(1) global ptr addrspace(1)

define i1 @test(i1 %cond) gc "hotspotgc" {
entry:
  %obj1 = load ptr addrspace(1), ptr addrspace(1) @glob1, !java-klass !0
  %obj2 = load ptr addrspace(1), ptr addrspace(1) @glob2, !java-klass !1
  %obj = select i1 %cond, ptr addrspace(1) %obj1, ptr addrspace(1) %obj2
  %result = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 1 to ptr),
    ptr addrspace(1) nonnull %obj)
  ret i1 %result
}

!0 = !{i64 11}
!1 = !{i64 12}

; CHECK: ret i1 true

!java-method-compilation = !{}
