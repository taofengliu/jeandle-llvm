; RUN: opt -passes=safepoint-elimination -S < %s | FileCheck %s

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
