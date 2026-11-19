; RUN: opt -S -passes=jeandle-inline-driver -verify-each -jeandle-vm-callback-log=%S/Inputs/exception-handler-scopes.cblog %s | FileCheck %s
; RUN: opt -S -passes=jeandle-inline-driver -verify-each -jeandle-vm-callback-log=%S/Inputs/exception-handler-denied.cblog %s | FileCheck %s --check-prefix=NO-INLINE
;
; A shared leaf is reached through two levels of callers, with and without
; exception handlers. Specialize only root copies, using each complete chain.

define hotspotcc void @root() #0 gc "hotspotgc" {
entry:
  %slot = alloca i64
  %a = call hotspotcc i1 @without_handler() #5 [ "deopt"(i32 10, i32 10, i64 327695, ptr %slot) ]
  call void @observe(i32 1, i1 %a)
  %b = call hotspotcc i1 @with_handler() #5 [ "deopt"(i32 20, i32 20, i64 327695, ptr %slot) ]
  call void @observe(i32 2, i1 %b)
  %own = call i1 @jeandle.has_exception_handler()
  call void @observe(i32 3, i1 %own)
  ret void
}

define available_externally hotspotcc i1 @without_handler() #1 gc "hotspotgc" {
  %slot = alloca i64
  %result = call hotspotcc i1 @middle() #5 [ "deopt"(i32 1, i32 1, i64 327695, ptr %slot) ]
  ret i1 %result
}

define available_externally hotspotcc i1 @with_handler() #2 gc "hotspotgc" {
  %slot = alloca i64
  %result = call hotspotcc i1 @middle() #5 [ "deopt"(i32 2, i32 2, i64 327695, ptr %slot) ]
  ret i1 %result
}

define available_externally hotspotcc i1 @middle() #3 gc "hotspotgc" {
  %slot = alloca i64
  %result = call hotspotcc i1 @leaf() #5 [ "deopt"(i32 3, i32 3, i64 327695, ptr %slot) ]
  ret i1 %result
}

define available_externally hotspotcc i1 @leaf() #4 gc "hotspotgc" {
  %result = call i1 @jeandle.has_exception_handler()
  ret i1 %result
}

define i1 @uncompiled_helper() {
  %result = call i1 @jeandle.has_exception_handler()
  ret i1 %result
}

declare i1 @jeandle.has_exception_handler() nounwind willreturn
declare void @observe(i32, i1)

attributes #0 = { "java-method"="1" }
attributes #1 = { "java-method"="101" }
attributes #2 = { "java-method"="102" "java-has-exception-handlers" }
attributes #3 = { "java-method"="103" }
attributes #4 = { "java-method"="104" }
attributes #5 = { "monomorphic-target" }

!java-method-compilation = !{}
!static-call-patch-size = !{!0}
!dynamic-call-patch-size = !{!1}
!0 = !{i32 5}
!1 = !{i32 15}

; CHECK-LABEL: define hotspotcc void @root(
; CHECK-NOT: @jeandle.has_exception_handler
; CHECK: call void @observe(i32 1, i1 false)
; CHECK: call void @observe(i32 2, i1 true)
; CHECK: call void @observe(i32 3, i1 false)
; CHECK-NEXT: ret void
; CHECK-LABEL: define i1 @uncompiled_helper(
; CHECK: call i1 @jeandle.has_exception_handler()

; NO-INLINE-LABEL: define hotspotcc void @root(
; NO-INLINE: call hotspotcc i1 @without_handler()
; NO-INLINE: call hotspotcc i1 @with_handler()
; NO-INLINE: call void @observe(i32 3, i1 false)
; NO-INLINE-LABEL: define i1 @uncompiled_helper(
; NO-INLINE: call i1 @jeandle.has_exception_handler()
