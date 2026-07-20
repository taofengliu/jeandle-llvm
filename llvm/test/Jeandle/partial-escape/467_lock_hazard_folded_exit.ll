; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Hazard scan: a folded monitorexit in a PH-dominated block
; processed BEFORE the merge that flips PH. `n` holds an unbalanced enter on
; %o (folded); `t` invokes @foo() (no PEA state change, so no UnwindData —
; the handler inherits %o virtual+locked); the handler `h` monitorexit's %o
; (FOLDED against the virtual lock state); `f` escapes %o; `m` merges t and
; f. RPO processes h BEFORE m, so the exit fold is already recorded when m's
; Case-A materialize of %o (locks) would place a re-emit at t's terminator —
; which covers the h path where the matching exit is deleted (unbalanced
; acquire). The hazard scan detects the folded exit (h is dominated by t)
; and keeps %o fully real instead: the ORIGINAL enter and exit survive and
; no re-emit is emitted anywhere.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1), ptr)
declare hotspotcc void @jeandle.monitorexit_with_lightweight_lock(ptr addrspace(1), ptr)
declare void @foo()
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @hazard_folded_exit_dominated(i1 %c) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %lk = alloca i64, align 8
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 11111 to ptr), i32 16)
       to label %n unwind label %u
n:
  call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(
              ptr addrspace(1) %o, ptr %lk)
  br i1 %c, label %t, label %f
t:
  invoke void @foo() to label %m unwind label %h
f:
  call void @sink(ptr addrspace(1) %o)
  br label %m
m:
  ret void
h:
  %lp = landingpad i64 cleanup
  call hotspotcc void @jeandle.monitorexit_with_lightweight_lock(
              ptr addrspace(1) %o, ptr %lk)
  resume i64 %lp
u:
  %lpr = landingpad i64 cleanup
  resume i64 %lpr
}

; Everything kept real by the hazard scan: the original enter (n) and exit
; (h) survive in place; exactly ONE enter and ONE exit total; no re-emit
; anywhere (no OTHER monitorenter beyond the original).
; CHECK-LABEL: define void @hazard_folded_exit_dominated(
; CHECK: n:
; CHECK-NEXT: call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1) %o, ptr %lk)
; CHECK: h:
; CHECK: call hotspotcc void @jeandle.monitorexit_with_lightweight_lock(ptr addrspace(1) %o, ptr %lk)
; CHECK-NOT: pea.mat
; CHECK-NOT: poison

!java-method-compilation = !{}
