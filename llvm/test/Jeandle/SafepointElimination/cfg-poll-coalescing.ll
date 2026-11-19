; RUN: opt -jeandle-enable-branch-poll-coalescing -verify-each -passes='safepoint-poll-elimination' -S %s | FileCheck %s
; RUN: opt -jeandle-enable-branch-poll-coalescing -verify-each -passes='safepoint-poll-elimination<after-strip-mining>,safepoint-poll-elimination<after-strip-mining>' -S %s | FileCheck %s
; RUN: opt -jeandle-enable-branch-poll-coalescing -jeandle-safepoint-max-work=0 -passes=safepoint-poll-elimination -S %s | FileCheck %s --check-prefix=LIMIT
; RUN: opt -jeandle-enable-branch-poll-coalescing -jeandle-safepoint-max-work=22 -passes=safepoint-poll-elimination -S %s | FileCheck %s --check-prefix=LIMIT

declare hotspotcc void @jeandle.safepoint_poll()
declare void @java_call()
declare void @opaque() nounwind "gc-leaf-function"
declare void @throwing() willreturn "gc-leaf-function"
declare void @leaf() nounwind willreturn "gc-leaf-function"
declare void @llvm.experimental.guard(i1, ...)
declare i32 @personality(...)

; A chain of deletion proofs must end at a poll that survives the transaction.
define void @chain() "java-method" {
entry:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 1)]
  br label %middle
middle:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 2)]
  br label %exit
exit:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 3)]
  ret void
}
; CHECK-LABEL: define void @chain(
; CHECK-NOT: @jeandle.safepoint_poll
; CHECK: exit:
; CHECK-NEXT: call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i32 3) ]
; CHECK-NEXT: ret void
; LIMIT-LABEL: define void @chain(
; LIMIT: "deopt"(i32 1)
; LIMIT: "deopt"(i32 2)
; LIMIT: "deopt"(i32 3)

; Neither arm dominates entry: their union is the necessary safepoint cut.
define void @diamond(i1 %choose) "java-method" {
entry:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 1)]
  br i1 %choose, label %left, label %right
left:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 2)]
  ret void
right:
  call void @java_call() ["deopt"()]
  br label %exit
exit:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 3)]
  ret void
}
; CHECK-LABEL: define void @diamond(
; CHECK-NOT: @jeandle.safepoint_poll
; CHECK: left:
; CHECK-NEXT: call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i32 2) ]
; CHECK: right:
; CHECK: call void @java_call()
; CHECK-NOT: @jeandle.safepoint_poll
; CHECK: ret void

define void @uncovered_arm(i1 %choose) "java-method" {
entry:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 1)]
  br i1 %choose, label %left, label %exit
left:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 2)]
  ret void
exit:
  ret void
}
; CHECK-LABEL: define void @uncovered_arm(
; CHECK: "deopt"(i32 1)
; CHECK: "deopt"(i32 2)

define void @switch_cut(i32 %choice) "java-method" {
entry:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 1)]
  switch i32 %choice, label %last [i32 0, label %first
                                  i32 1, label %first]
first:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 2)]
  ret void
last:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 3)]
  ret void
}
; CHECK-LABEL: define void @switch_cut(
; CHECK-NOT: @jeandle.safepoint_poll
; CHECK: switch i32
; CHECK: "deopt"(i32 2)
; CHECK: "deopt"(i32 3)

; A non-returning leaf cannot justify reaching the later guaranteed call.
define void @nonreturn_barrier() "java-method" {
entry:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 1)]
  call void @opaque()
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 2)]
  call void @java_call() ["deopt"()]
  ret void
}
; CHECK-LABEL: define void @nonreturn_barrier(
; CHECK: "deopt"(i32 1)
; CHECK-NEXT: call void @opaque()
; CHECK-NEXT: call void @java_call()

define void @throw_barrier() "java-method" {
entry:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 1)]
  call void @throwing()
  br label %exit
exit:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 2)]
  ret void
}
; CHECK-LABEL: define void @throw_barrier(
; CHECK: "deopt"(i32 1)
; CHECK: call void @throwing()
; CHECK: "deopt"(i32 2)

define void @transparent_leaf() "java-method" {
entry:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 1)]
  call void @leaf()
  br label %exit
exit:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 2)]
  ret void
}
; CHECK-LABEL: define void @transparent_leaf(
; CHECK-NOT: @jeandle.safepoint_poll
; CHECK: call void @leaf()
; CHECK: "deopt"(i32 2)

; Forward call coverage also stops at an opaque operation.
define void @prior_call_barrier() "java-method" {
entry:
  call void @java_call() ["deopt"()]
  call void @opaque()
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 1)]
  ret void
}
; CHECK-LABEL: define void @prior_call_barrier(
; CHECK: call void @opaque()
; CHECK-NEXT: call hotspotcc void @jeandle.safepoint_poll()

; A successful guard carries deopt state but does not service requests.
define void @guard_is_not_coverage(i1 %condition) "java-method" {
entry:
  call void (i1, ...) @llvm.experimental.guard(i1 %condition) ["deopt"()]
  call hotspotcc void @jeandle.safepoint_poll()
  ret void
}
; CHECK-LABEL: define void @guard_is_not_coverage(
; CHECK: call void (i1, ...) @llvm.experimental.guard
; CHECK-NEXT: call hotspotcc void @jeandle.safepoint_poll()

; The marker is part of the strip-mined bound proof, even outside a loop.
define void @marked_poll() "java-method" {
entry:
  call hotspotcc void @jeandle.safepoint_poll() "jeandle.strip-mined-poll"
  call void @java_call() ["deopt"()]
  ret void
}
; CHECK-LABEL: define void @marked_poll(
; CHECK: call hotspotcc void @jeandle.safepoint_poll()
; CHECK: call void @java_call()

define void @marked_loop() "java-method" {
entry:
  br label %loop
loop:
  call hotspotcc void @jeandle.safepoint_poll() "jeandle.strip-mined-poll"
  call void @java_call() ["deopt"()]
  br label %loop
}
; CHECK-LABEL: define void @marked_loop(
; CHECK: call hotspotcc void @jeandle.safepoint_poll()
; CHECK-NEXT: call void @java_call()

; An outside proof cannot use a poll which mining or empty-loop deletion may
; subsequently remove. Nor may it reach a poll by crossing a natural backedge.
define void @ownership_boundary(i1 %keep) "java-method" {
entry:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 1)]
  br label %loop
loop:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 2)]
  br i1 %keep, label %loop, label %exit
exit:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 3)]
  ret void
}
; CHECK-LABEL: define void @ownership_boundary(
; CHECK: "deopt"(i32 1)
; CHECK: "deopt"(i32 2)
; CHECK: "deopt"(i32 3)

; Both successors of a guaranteed invoke have already passed a safepoint.
define void @invoke_coverage() "java-method" personality ptr @personality {
entry:
  invoke void @java_call() ["deopt"()] to label %normal unwind label %handler
normal:
  call hotspotcc void @jeandle.safepoint_poll()
  ret void
handler:
  %exception = landingpad {ptr, i32} cleanup
  call hotspotcc void @jeandle.safepoint_poll()
  resume {ptr, i32} %exception
}
; CHECK-LABEL: define void @invoke_coverage(
; CHECK: invoke void @java_call()
; CHECK-NOT: @jeandle.safepoint_poll
; CHECK: resume

!java-method-compilation = !{}
