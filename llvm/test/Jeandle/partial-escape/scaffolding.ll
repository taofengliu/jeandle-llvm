; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Scaffolding test: PEA is a no-op. The IR should be unchanged.

define i32 @trivial(i32 %x) gc "hotspotgc" {
entry:
  %r = add i32 %x, 1
  ret i32 %r
}

; CHECK-LABEL: define i32 @trivial(i32 %x)
; CHECK:         %r = add i32 %x, 1
; CHECK:         ret i32 %r

!java-method-compilation = !{}
