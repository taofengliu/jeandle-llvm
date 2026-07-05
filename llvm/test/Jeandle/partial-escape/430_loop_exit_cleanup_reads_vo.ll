; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; processLoopExit 0-clause cleanup that observes a VO.
;
; %o is a loop-local virtual; the loop body's may_throw invoke unwinds to a
; 0-clause cleanup (%cleanup) that READS %o's field. The cleanup is not a pure
; `landingpad; resume` OOM handler, so processLoopExit must treat it as an
; exception-handling exit and force %o material by the time the handler runs.
; (The pre-fix clause-count heuristic skipped ALL 0-clause landingpads on the
; unverified assumption that they only resume.)

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @may_throw()
declare void @use(i32)
declare i32 @__gxx_personality_v0(...)

define void @test_loop_exit_cleanup_reads_vo(i1 %cond) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
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
  %lp = landingpad i64 cleanup
  %fv = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  %r = load atomic i32, ptr addrspace(1) %fv unordered, align 4
  call void @use(i32 %r)
  resume i64 %lp
}

; CHECK-LABEL: define void @test_loop_exit_cleanup_reads_vo
; %o is materialized (it must be real when %cleanup reads its field), and the
; cleanup's load survives reading the real field.
; CHECK: invoke hotspotcc{{.*}}@jeandle.new_instance
; CHECK: load atomic i32
; CHECK: call void @use

!java-method-compilation = !{}
