; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Multi-scope bundle guard (review §3 #7): the transform's slot rewrite and
; descriptor insert cover only the INNERMOST (current-method) scope, and the
; JDK parser's vo_map is per-scope. A VO referenced ONLY by an OUTER scope
; (here: the root scope's locals slot) must not be described — pre-fix it
; was collected as a root, DeoptBundleHandled suppressed the generic escape,
; the transform bailed (OrigAlloc absent from the innermost scan range), and
; Pass 2 poisoned the outer-scope slot. Now such a VO is excluded from
; description and materialized at the call: the live OrigAlloc oop is valid
; in EVERY scope.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(i32)
declare i32 @__gxx_personality_v0(...)

define void @multiscope(i32 %x) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 100 to ptr), i32 16)
       to label %n1 unwind label %u
n1:
  %of = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  store atomic i32 %x, ptr addrspace(1) %of unordered, align 4
  ; Two-scope bundle: ROOT scope (bci 5) references %o in its locals;
  ; the innermost (current) scope (bci 9) does NOT.
  ; Layout per scope: [method], should_reexecute(i64), bci, bci, locals, stack, monitors, [orig_pc]
  call void @sink(i32 %x)
       [ "deopt"(i32 5, i32 5, i64 12, ptr addrspace(1) %o,
                 i64 262147, i64 777,
                 i64 1, i32 9, i32 9, i64 10, i32 %x) ]
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; %o is PartiallyEscapes (materialized at the call): OrigAlloc retained,
; field replayed; the outer-scope slot keeps the LIVE %o oop; NO descriptor
; and NO VORef anywhere.
; CHECK-LABEL: define void @multiscope(
; CHECK: invoke hotspotcc ptr addrspace(1) @jeandle.new_instance
; CHECK: store atomic i32 %x, ptr addrspace(1) %pea.matslot unordered, align 4
; CHECK: call void @sink(i32 %x) [ "deopt"(i32 5, i32 5, i64 12, ptr addrspace(1) %o,
; CHECK-NOT: i64 262156
; CHECK-NOT: i64 524300
; CHECK-NOT: poison

!java-method-compilation = !{}
