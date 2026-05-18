; RUN: not opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/invoke-check-instanceof.cblog %s 2>&1 | FileCheck %s

; Test: `invoke` instead of `call` to check_instanceof.
; BUG: The pass uses CallBase (covers both call and invoke) to detect checks,
; but eraseFromParent() on an invoke removes the terminator, crashing the
; verifier. The pass should replace the invoke with a br to the normal
; destination instead of erasing it.
; This test documents the current buggy behavior (crash).

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @test(ptr addrspace(1) nonnull "java-klass"="7" "java-klass-exact" %obj) gc "hotspotgc"
    personality ptr @__gxx_personality_v0 {
entry:
  %check = invoke i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 6 to ptr),
    ptr addrspace(1) nonnull %obj)
    to label %normal unwind label %exception

normal:
  ret i1 %check

exception:
  %lp = landingpad { ptr, i32 } cleanup
  ret i1 false
}

declare i32 @__gxx_personality_v0(...)

; CHECK: Basic Block in function 'test' does not have terminator!

!java-method-compilation = !{}
