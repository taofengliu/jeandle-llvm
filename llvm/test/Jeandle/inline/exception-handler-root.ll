; RUN: opt -S -passes=jeandle-inline-driver -verify-each -jeandle-vm-callback-log=%S/Inputs/exception-handler-denied.cblog %s | FileCheck %s
;
; The root method itself also counts, without requiring any successful inlines.

define hotspotcc i1 @root() #0 gc "hotspotgc" {
  %result = call i1 @jeandle.has_exception_handler()
  ret i1 %result
}

declare i1 @jeandle.has_exception_handler() nounwind willreturn

attributes #0 = { "java-method"="1" "java-has-exception-handlers" }
!java-method-compilation = !{}

; CHECK-LABEL: define hotspotcc i1 @root(
; CHECK-NEXT: ret i1 true
