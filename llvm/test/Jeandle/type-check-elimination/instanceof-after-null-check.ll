; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/instanceof-after-null-check.cblog %s 2>&1 | FileCheck %s

; Test: Post-inlining pattern from jeandle.instanceof template.
; After JavaOperationLower(0) + InstSimplify, the typical instanceof pattern is:
;   null check → true branch calls check_instanceof → zext to i32
; Object type is known from param attribute: Dog (7, final) → fold to true for Animal (6).

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i32 @instanceof_animal(ptr addrspace(1) "java-klass"="7" "java-klass-exact" %obj) gc "hotspotgc" {
entry:
  %is_null = icmp eq ptr addrspace(1) %obj, null
  br i1 %is_null, label %null_path, label %non_null

non_null:
  %check = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 6 to ptr),
    ptr addrspace(1) nonnull %obj)
  %result = zext i1 %check to i32
  br label %merge

null_path:
  br label %merge

merge:
  %phi = phi i32 [ %result, %non_null ], [ 0, %null_path ]
  ret i32 %phi
}

; CHECK-LABEL: @instanceof_animal
; CHECK: non_null:
; CHECK-NEXT: %result = zext i1 true to i32
; CHECK-NEXT: br label %merge
; CHECK: merge:
; CHECK-NEXT: %phi = phi i32 [ %result, %non_null ], [ 0, %null_path ]

!java-method-compilation = !{}
