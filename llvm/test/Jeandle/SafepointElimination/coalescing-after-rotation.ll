; RUN: opt -verify-each -passes='loop-simplify,lcssa,loop(loop-rotate)' -S %s -o %t.rotated
; RUN: FileCheck %s --check-prefix=ROTATE < %t.rotated
; RUN: opt -verify-each -passes=loop-unroll -S %t.rotated -o %t.unrolled
; RUN: FileCheck %s --check-prefix=UNROLL < %t.unrolled
; RUN: opt -jeandle-enable-branch-poll-coalescing -verify-each -passes='safepoint-poll-elimination,verify<jeandle-safepoint-coverage>' -S %t.unrolled | FileCheck %s --check-prefix=FINAL

; Rotation copies the header poll into the entry and the new latch. After
; full unrolling, their blocks are acyclic and only the final cut is needed.
; ROTATE-LABEL: define void @rotated(
; ROTATE: entry:
; ROTATE-NEXT: call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i32 0) ]
; ROTATE: body:
; ROTATE: %i1 = phi i32
; ROTATE: "deopt"(i32 %next)
; UNROLL-LABEL: define void @rotated(
; UNROLL-COUNT-5: call hotspotcc void @jeandle.safepoint_poll()
; UNROLL-NOT: call hotspotcc void @jeandle.safepoint_poll()
; UNROLL: ret void
; FINAL-LABEL: define void @rotated(
; FINAL-NOT: call hotspotcc void @jeandle.safepoint_poll()
; FINAL: call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i32 30) ]
; FINAL-NEXT: ret void

declare hotspotcc void @jeandle.safepoint_poll()
define void @rotated(ptr %out) "java-method" {
entry:
 br label %head
head:
 %i = phi i32 [0,%entry], [%next,%body]
 call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 %i)]
 %keep = icmp ult i32 %i,3
 br i1 %keep,label %body,label %exit
body:
 store i32 %i,ptr %out
 %next = add nuw nsw i32 %i,1
 br label %head, !llvm.loop !0
exit:
 call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 30)]
 ret void
}
!java-method-compilation = !{}
!0 = distinct !{!0,!1}
!1 = !{!"llvm.loop.unroll.full"}
