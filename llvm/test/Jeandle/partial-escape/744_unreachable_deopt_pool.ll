; RUN: opt -S -verify-each \
; RUN:   -passes="require<partial-escape-analysis>,partial-escape-transform" \
; RUN:   %s | FileCheck %s
; RUN: opt -disable-output -passes="require<partial-escape-analysis>" \
; RUN:   -jeandle-trace-pea -jeandle-dump-pea-stats \
; RUN:   -jeandle-pea-analyze-function=stable_empty_pool %s 2>&1 \
; RUN:   | FileCheck %s --check-prefix=NOOP-TRACE \
; RUN:       --implicit-check-not='PEA: RewriteDeoptPool function=@stable_empty_pool'

; Existing descriptors are a safepoint-local object graph, not append-only
; history.  PEA canonicalizes that graph even when the current round contributes
; no new virtual object: unreachable legacy nodes are discarded and every
; surviving descriptor edge and scope root is rewritten to the new dense wire
; ID.

declare void @safepoint()

; A safepoint without an object pool is already canonical.  Analysis must not
; publish a rewrite effect solely to copy an unchanged deopt bundle.
define void @stable_empty_pool() gc "hotspotgc" {
entry:
  call void @safepoint() [ "deopt"(i32 743, i32 743) ]
  ret void
}

; NOOP-TRACE: ;; PEA stats @stable_empty_pool: NeverEscapes=0 PartiallyEscapes=0 AlwaysEscapes=0

; A pool with no roots has no reachable nodes.  Cleanup must remove the complete
; descriptor, including all of its fields, while preserving the scope prefix.
define void @cleanup_fully_unreachable_pool() gc "hotspotgc" {
entry:
  call void @safepoint()
      [ "deopt"(i32 744, i32 744,
               ; Legacy wire ID 7, Point { int x = 17 }.
               i64 30065033228, i64 74401, i32 1,
               i64 34359738378, i32 17) ]
  ret void
}

; CHECK-LABEL: define void @cleanup_fully_unreachable_pool()
; CHECK: call void @safepoint() [ "deopt"(i32 744, i32 744) ]{{$}}

; Wire ID 7 is reachable from both a local and an expression-stack root and
; contains a self-reference.  Wire ID 9 has no incoming edge.  Canonicalization
; keeps only the first descriptor, renumbers it to ID 0, and rewrites its field
; edge and both kinds of scope root consistently.
define void @cleanup_sparse_reachable_pool() gc "hotspotgc" {
entry:
  call void @safepoint()
      [ "deopt"(i32 745, i32 745,
               ; Reachable legacy wire ID 7, Node { Node next = this }.
               i64 30065033228, i64 74402, i32 1,
               i64 34360262668, i32 7,
               ; Unreachable legacy wire ID 9.
               i64 38654967820, i64 74403, i32 0,
               ; Local and stack roots both refer to wire ID 7.
               i64 30065295372, i32 7,
               i64 30065360908, i32 7) ]
  ret void
}

; CHECK-LABEL: define void @cleanup_sparse_reachable_pool()
; Match the complete bundle: exactly one descriptor survives, with no stale
; wire ID 7/9 references or trailing descriptor operands.
; CHECK: call void @safepoint() [ "deopt"(i32 745, i32 745, i64 262156, i64 74402, i32 1, i64 34360262668, i32 0, i64 524300, i32 0, i64 589836, i32 0) ]{{$}}

!java-method-compilation = !{}
