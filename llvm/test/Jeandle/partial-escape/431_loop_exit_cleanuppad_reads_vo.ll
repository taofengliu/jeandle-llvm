; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; processLoopExit EH-pad successor coverage: a cleanuppad-headed loop-exit
; successor. %o is a loop-local virtual; the loop body's may_throw invoke
; unwinds to a cleanuppad (%cleanup) that READS %o's field. A cleanuppad body
; is not constrained to resume, so processLoopExit must treat it as an
; exception-handling exit and force %o to be materialised by the time the
; handler runs — exactly as it does for a landingpad/catchpad successor.
;
; Without cleanuppad detection (isEHPad()), %o would be eliminated and the
; cleanup's load folded to the stored constant, leaving the handler with no
; real object. The personality is the standard landingpad one (__gxx_personality_v0)
; so the materialise's landingpad unwind-dest is valid IR; cleanuppad itself
; verifies under it, exercising the analysis-side detection directly.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @may_throw()
declare void @use(i32)
declare i32 @__gxx_personality_v0(...)

define void @test_loop_exit_cleanuppad_reads_vo(i1 %cond) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  br label %loop
loop:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
  ptr inttoptr (i64 12345 to ptr), i32 32)
  to label %body unwind label %oom
body:
  %f = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  store atomic i32 42, ptr addrspace(1) %f unordered, align 4
  invoke void @may_throw() to label %back unwind label %cleanup
back:
  br i1 %cond, label %loop, label %exit
exit:
  ret void
oom:
  %lp0 = landingpad i64 cleanup
  resume i64 %lp0
cleanup:
  %cp = cleanuppad within none []
  %fv = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  %r = load atomic i32, ptr addrspace(1) %fv unordered, align 4
  call void @use(i32 %r)
  cleanupret from %cp unwind to caller
}

; CHECK-LABEL: define void @test_loop_exit_cleanuppad_reads_vo
; %o is materialised (it must be real when %cleanup reads its field), and the
; cleanup's load survives reading the real field.
; CHECK: invoke hotspotcc{{.*}}@jeandle.new_instance
; CHECK: load atomic i32
; CHECK: call void @use

!java-method-compilation = !{}
