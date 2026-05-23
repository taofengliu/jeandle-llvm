; RUN: opt -S -passes="partial-escape-iterative" -jeandle-pea-iterations=4 %s | FileCheck %s

; D1 convergence detection. Same input as 280 (two-round elimination) but
; runs with iterations=4. We expect the wrapper to converge in exactly
; two rounds (round 1 materializes, canonicalize folds the dead branch,
; round 2 eliminates the alloc), then detect convergence and short-circuit
; the cap.
;
; The behavioural assertion is exactly the same as 280: the alloc and the
; sink call must be gone, and the function reduces to `ret i32 42`.
; If early-exit were broken the wrapper would still produce the same IR
; (round 3 / 4 would simply be no-ops on the already-converged IR), so
; this test cannot directly verify the early-exit; what it DOES verify is
; that running with a larger cap produces the same IR as 280 and doesn't
; regress (e.g. by re-materializing something we just eliminated).

@G_zero = private unnamed_addr constant i32 0

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define i32 @test_convergence_detection()
    gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %n unwind label %u
n:
  %v = load i32, ptr @G_zero, align 4
  %c = icmp ne i32 %v, 0
  br i1 %c, label %escape, label %fast
escape:
  call void @sink(ptr addrspace(1) %o)
  br label %fast
fast:
  %slot = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  store atomic i32 42, ptr addrspace(1) %slot unordered, align 4
  %val = load atomic i32, ptr addrspace(1) %slot unordered, align 4
  ret i32 %val
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define i32 @test_convergence_detection()
; CHECK-NOT: jeandle.new_instance
; CHECK-NOT: call void @sink
; CHECK: ret i32 42

!java-method-compilation = !{}
