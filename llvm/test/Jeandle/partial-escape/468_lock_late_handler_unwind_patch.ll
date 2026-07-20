; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Late/shared handler + merge-driven UnwindData patch. A
; locked VO %o is virtual on the t arm (invoke @foo(%p) escapes %p, so t's
; pre-invoke snapshot is stashed as UnwindData) and materialized on the f
; arm (sink(%o)). The merge at m does a Case-A materialize of %o with locks
; at t's INVOKE terminator, and h is a SHARED handler (unwind pred from t
; and from z inside m's subtree) processed AFTER m in RPO. The merge-driven
; patch marks %o materialized (locks cleared) in t's stashed UnwindData, so
; h's monitorexit sees a real object and survives as a real exit —
; balancing the re-emitted enter on the unwind path. Without it, h would
; see %o still virtual+locked and fold the exit away (leak).

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1), ptr)
declare hotspotcc void @jeandle.monitorexit_with_lightweight_lock(ptr addrspace(1), ptr)
declare void @foo(ptr addrspace(1))
declare void @bar()
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @late_handler(i1 %c) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %lk = alloca i64, align 8
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 11111 to ptr), i32 16)
       to label %n unwind label %u
n:
  call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(
              ptr addrspace(1) %o, ptr %lk)
  %p = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 22222 to ptr), i32 16)
       to label %np unwind label %u
np:
  br i1 %c, label %t, label %f
t:
  invoke void @foo(ptr addrspace(1) %p) to label %m unwind label %h
f:
  call void @sink(ptr addrspace(1) %o)
  br label %m
m:
  br label %z
z:
  invoke void @bar() to label %zok unwind label %h
zok:
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

; The exit at h SURVIVES as a real exit (the patched UnwindData shows %o
; materialized, so the fold cannot fire). Re-emitted enters land on each
; path exactly once (t-invoke for the t->h unwind path, f's sink for the
; f->m->z path — disjoint, both kept).
; CHECK-LABEL: define void @late_handler(
; CHECK: call hotspotcc void @jeandle.monitorexit_with_lightweight_lock(ptr addrspace(1) %o, ptr %lk)
; CHECK-NOT: pea.mat
; CHECK-NOT: poison

!java-method-compilation = !{}
