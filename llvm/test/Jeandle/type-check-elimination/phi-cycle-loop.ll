; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/phi-cycle-loop.cblog %s 2>&1 | FileCheck %s

; Test: PHI with self-referencing cycle from loop back-edge. getBaseJavaType
; detects that the PHI itself appears as an incoming (via Visited set) and
; skips it, determining type from non-cyclic incomings only.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @test(ptr addrspace(1) nonnull "java-klass"="7" "java-klass-exact" %obj) gc "hotspotgc" {
entry:
  br label %loop

loop:
  ; PHI with self-cycle: entry → %obj (Dog, exact), back-edge → %current (self).
  ; getBaseJavaType sees %current in Visited → skips. Uses %obj → {7, exact}.
  %current = phi ptr addrspace(1) [ %obj, %entry ], [ %current, %loop ]
  ; Check: is current instanceof Animal (6)? Dog(7, exact) → fold true.
  %check = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) inttoptr (i64 6 to ptr),
    ptr addrspace(1) nonnull %current)
  br i1 %check, label %loop, label %exit

exit:
  ret i1 %check
}

; CHECK-LABEL: @test
; CHECK: loop:
; CHECK-NEXT: %current = phi
; CHECK-NEXT: br i1 true, label %loop, label %exit

!java-method-compilation = !{}
