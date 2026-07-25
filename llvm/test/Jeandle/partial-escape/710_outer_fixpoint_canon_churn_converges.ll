; RUN: opt -S -passes="partial-escape-iterative" -jeandle-pea-iterations=8 -jeandle-dump-pea-ir=stable_convergence %s 2>&1 | FileCheck %s

; Regression test for the outer fixpoint convergence logic. A function whose
; PEA-specific signals (transform idle, alloc count, virtualization delta,
; allocation delta) are stable from round 1 onward — but whose canonicalization
; (SimplifyCFG/LoopSimplify) reports "changed" every round due to conservative
; areAllPreserved — must still converge. The PEAStableRounds mechanism
; declares convergence after 2 consecutive PEA-stable rounds.

declare hotspotcc void @jeandle.safepoint_poll()
declare i32 @__gxx_personality_v0(...)

define i32 @stable_convergence(i32 %0) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %OrigPcSlot = alloca i64, align 8
  %.not20 = icmp sgt i32 %0, 0
  br i1 %.not20, label %loop.preheader, label %exit

loop.preheader:
  br label %loop

exit.loopexit:
  br label %exit

exit:
  %.lcssa11 = phi i32 [ 0, %entry ], [ %4, %exit.loopexit ]
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i32 0, i32 0, i64 99, i32 0, i64 65546, i32 %.lcssa11) ]
  ret i32 %.lcssa11

loop:
  %1 = phi i32 [ %5, %loop ], [ 0, %loop.preheader ]
  %2 = phi i32 [ %4, %loop ], [ 0, %loop.preheader ]
  %pea.field.phi = phi i32 [ %3, %loop ], [ 0, %loop.preheader ]
  %3 = add i32 %pea.field.phi, %1
  %4 = add i32 %2, %3
  %5 = add nuw nsw i32 %1, 1
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i32 0, i32 0, i64 65546, i32 %4) ]
  %exitcond.not = icmp eq i32 %5, %0
  br i1 %exitcond.not, label %exit.loopexit, label %loop
}

; The outer fixpoint must converge (stop=fixpoint), not hit the iteration cap.
; CHECK: PEA-SUMMARY function stable_convergence rounds={{[1-4]}} stop=fixpoint

!java-method-compilation = !{}
