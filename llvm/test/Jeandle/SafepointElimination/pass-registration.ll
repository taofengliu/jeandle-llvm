; RUN: opt -passes=safepoint-elimination -S < %s | FileCheck %s
; RUN: opt -passes='safepoint-elimination<early>' -S < %s | FileCheck %s
; RUN: opt -passes='safepoint-elimination<cleanup>' -S < %s | FileCheck %s
; RUN: opt -passes='safepoint-elimination<strip-mining>' \
; RUN:   -jeandle-enable-strip-mining -S < %s | FileCheck %s
; RUN: opt --print-passes | FileCheck %s --check-prefix=PRINT

; The pass resolves by name as a function pass and leaves IR intact.

declare hotspotcc void @jeandle.safepoint_poll()

define void @f() gc "no-op" {
entry:
  call hotspotcc void @jeandle.safepoint_poll()
  ret void
}

!java-method-compilation = !{}

; CHECK-LABEL: @f(
; CHECK: call hotspotcc void @jeandle.safepoint_poll()
; PRINT: safepoint-elimination<early;inclusive-loop-versioning;strip-mining;cleanup;loop-deletion-prep>
