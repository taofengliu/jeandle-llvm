; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/addrspacecast-on-obj.cblog %s 2>&1 | FileCheck %s

; Test: Object operand wrapped in freeze + bitcast.
; getJavaType's stripPointerCastsAndAliases (at top level) and getBaseJavaType's
; explicit Freeze/BitCast pass-through find the underlying typed value.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @test(ptr addrspace(1) nonnull "java-klass"="7" "java-klass-exact" %obj) gc "hotspotgc" {
entry:
  %frozen = freeze ptr addrspace(1) %obj
  %check = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 6 to ptr),
    ptr addrspace(1) nonnull %frozen)
  ret i1 %check
}

; CHECK-LABEL: @test
; CHECK: ret i1 true

!java-method-compilation = !{}
