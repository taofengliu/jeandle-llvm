; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/dominator-non-edge-dominance.cblog %s 2>&1 | FileCheck %s

; Test: Edge dominance vs block dominance distinction.
; Block "merge" has two predecessors (true_path and false_path), both from
; the same branch. Neither edge individually dominates "merge", so no
; sharpening should apply even though the branch's true-edge successor
; dominates "merge" in the block-dominance sense... actually in this case
; true_path is the only predecessor of merge with the check, and if
; merge has multiple predecessors from both branches, no edge dominates.
;
; Specifically: br i1 %is_dog → true_path or false_path. Both jump to merge.
; Edge(entry, true_path) does NOT dominate merge (merge also reachable via false_path).
; So the check in merge should NOT be sharpened.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @test(ptr addrspace(1) nonnull %obj) gc "hotspotgc" {
entry:
  %is_dog = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 7 to ptr),
    ptr addrspace(1) nonnull %obj)
  br i1 %is_dog, label %true_path, label %false_path

true_path:
  br label %merge

false_path:
  br label %merge

merge:
  ; Neither edge from entry dominates merge (both paths reach here).
  ; obj type should NOT be sharpened → check preserved.
  %check = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 6 to ptr),
    ptr addrspace(1) nonnull %obj)
  ret i1 %check
}

; CHECK-LABEL: @test
; CHECK: merge:
; CHECK-NEXT: %check = call i1 @jeandle.check_instanceof

!java-method-compilation = !{}
