; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/klass-inttoptr-sext.cblog %s 2>&1 | FileCheck %s

; Test: SuperKlass encoded as inttoptr(sext i32 to i64). extractKlassConstant
; should see through the sext instruction to extract the constant.
; Hierarchy: obj has klass 5 (SubRunnable), checking instanceof klass 4 (MyRunnable).
; IsSubtype(5, 4) = true => fold to true.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

@glob = external addrspace(1) global ptr addrspace(1)

define i1 @test() gc "hotspotgc" {
entry:
  %obj = load ptr addrspace(1), ptr addrspace(1) @glob, !java-klass !0
  %narrow = sext i32 4 to i64
  %klass_ptr = inttoptr i64 %narrow to ptr
  %result = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) %klass_ptr,
    ptr addrspace(1) nonnull %obj)
  ret i1 %result
}

!0 = !{i64 5}

; CHECK: ret i1 true

!java-method-compilation = !{}
