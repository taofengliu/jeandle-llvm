; RUN: opt -passes='verify<jeandle-safepoint-coverage>' \
; RUN:   -jeandle-safepoint-coverage-print-only -S < %s 2>&1 | FileCheck %s

; An unbounded loop with no poll at all violates the coverage invariant: a
; thread inside it can never reach a safepoint. The verifier must report it
; (print-only keeps opt alive so the report is observable).

define void @naked_loop(i64 %n) gc "safepoint-in-loop-example" {
entry:
  br label %loop.header

loop.header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop.header ]
  %iv.next = add nsw i64 %iv, 1
  %exit.cond = icmp slt i64 %iv.next, %n
  br i1 %exit.cond, label %loop.header, label %exit

exit:
  ret void
}

!java-method-compilation = !{}

; CHECK: SafepointCoverageVerifier: loop with header 'loop.header' in function 'naked_loop' has no dominating safepoint poll and no provable trip bound
