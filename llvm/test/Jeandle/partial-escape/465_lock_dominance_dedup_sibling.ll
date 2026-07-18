; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Double-acquire on a sibling path processed before the merge (review §3 #4
; + the RPO twist). `n` holds an unbalanced enter on %o; `p` branches to
; merge1 and `s`; `q` escapes via foo(o); `s` escapes via baz(o). RPO is
; [entry, n, q, p, s, merge1]: s's baz(o) escape captures and would re-emit
; the SAME folded lock BEFORE merge1's shared-flip materialize at p's
; terminator captures it again — two acquires on the p->s path. The
; commit-time dominance dedup keeps only the copy whose InsertBefore
; dominates the other (p's terminator dominates s), so baz's re-emit is
; dropped: p->s acquires exactly once (at p), q acquires once (at foo).

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1), ptr)
declare void @foo(ptr addrspace(1))
declare void @baz(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @s_path(i1 %c0, i1 %c) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %lo = alloca i64, align 8
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 11111 to ptr), i32 16)
       to label %n unwind label %u
n:
  call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(
              ptr addrspace(1) %o, ptr %lo)
  br i1 %c0, label %p, label %q
p:
  br i1 %c, label %merge1, label %s
q:
  call void @foo(ptr addrspace(1) %o)
  br label %merge1
s:
  call void @baz(ptr addrspace(1) %o)
  br label %merge1
merge1:
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; Exactly two re-emitted enters survive: one at p's terminator (covers both
; p->merge1 and p->s), one at q's foo (covers q). NONE at s's baz (dropped
; by the dominance dedup).
; CHECK-LABEL: define void @s_path(
; CHECK: p:
; CHECK-NEXT: call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1) %o, ptr %lo)
; CHECK-NEXT: br i1 %c
; CHECK: q:
; CHECK-NEXT: call hotspotcc void @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1) %o, ptr %lo)
; CHECK-NEXT: call void @foo(ptr addrspace(1) %o)
; CHECK: s:
; CHECK-NEXT: call void @baz(ptr addrspace(1) %o)
; CHECK-NOT: monitorenter
; CHECK-NOT: poison

!java-method-compilation = !{}
