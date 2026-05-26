; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; processLoopExit force-materialises virtuals at any loop-exit whose
; successor is a real catch-handler EH pad (landingpad with at least one
; catch clause). In the loop below the EXIT block %check has an invoke
; whose unwind dest is a landingpad with an explicit clause.
; processLoopExit runs on %check and drains every still-virtual VO at
; the exit terminator. The alloc in the loop must survive in IR (the
; force-mat re-emits it).
;
; Pure-cleanup landingpads (clauses==0) are NOT considered EH-exit by
; the helper; that case is exercised throughout the existing test suite
; (every alloc IS an invoke with a cleanup unwind dest) and the gate at
; "LP->getNumClauses() > 0" keeps those paths from being demoted.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @may_throw(ptr addrspace(1))
declare void @catch_handler(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

@C = external addrspace(1) global ptr addrspace(1)

define void @test_loop_exit_eh(i32 %n) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  br label %hdr

hdr:
  %i = phi i32 [0, %entry], [%inext, %latch]
  %c = icmp slt i32 %i, %n
  br i1 %c, label %body, label %ret

body:
  %obj = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
                  ptr inttoptr (i64 8888 to ptr), i32 16)
              to label %check unwind label %u
check:
  invoke void @may_throw(ptr addrspace(1) %obj)
          to label %latch unwind label %catchlp

latch:
  %inext = add i32 %i, 1
  br label %hdr

ret:
  ret void

catchlp:
  ; Landingpad with one explicit catch clause — qualifies as a real EH
  ; exit, so processLoopExit force-materialises at %check.
  %lp = landingpad i64
          catch ptr null
  call void @catch_handler(ptr addrspace(1) %obj)
  resume i64 %lp

u:
  ; Pure cleanup landingpad — does NOT trigger force-materialisation.
  %lpc = landingpad i64 cleanup
  resume i64 %lpc
}

; The alloc must survive in IR because the EH-exit at %check forces
; materialisation. Either the original invoke survives or a materialised
; copy is emitted; either way the klass id appears in the IR.
; CHECK-LABEL: define void @test_loop_exit_eh
; CHECK: jeandle.new_instance({{.*}}i64 8888

!java-method-compilation = !{}
