; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Case-A CASCADE: two objects %o1 (array) and %o2 (instance), both virtual on
; `else` and absent on `then`, both Case-A-materialized at `else`'s terminator
; — a folded jeandle.arraylength invoke on %o1. The eager-update hook
; (`relocateDependentMaterializes`) re-aims BOTH cascade members' InsertBefore
; off the erased invoke onto the in-block successor. Without the eager update,
; both members' InsertBefore would be null and applyMaterialize's assert fires.
;
; Under reuse-OrigAlloc neither object is re-materialized: the ORIGINAL
; allocation invokes (OrigAlloc %o1 and OrigAlloc %o2) are both KEPT (no fresh
; pea.mat invokes). The folded arraylength invoke is erased, and `else`
; becomes a plain br to %merge. The merge PHIs and sink calls are preserved.

declare hotspotcc ptr addrspace(1) @jeandle.new_array(ptr, i32, i32, i32, i32)
declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc i32 @jeandle.arraylength(ptr addrspace(1) readonly)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @casea_folded_invoke_term_cascade(i1 %c) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o1 = invoke hotspotcc ptr addrspace(1) @jeandle.new_array(ptr inttoptr (i64 12345 to ptr), i32 7, i32 44, i32 16, i32 1048576) to label %n1 unwind label %u1
n1:
  %o2 = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 22222 to ptr), i32 16) to label %n2 unwind label %u2
n2:
  br i1 %c, label %then, label %else
then:
  br label %merge
else:
  ; arraylength reads %o1 (virtual) -> folds, erasing this invoke (else's
  ; terminator). Both %o1 and %o2 are virtual here, absent on `then` -> two
  ; Case-A materializes at the same erased terminator (cascade group).
  %len = invoke hotspotcc i32 @jeandle.arraylength(ptr addrspace(1) %o1) to label %merge unwind label %handler
merge:
  %p1 = phi ptr addrspace(1) [ null, %then ], [ %o1, %else ]
  %p2 = phi ptr addrspace(1) [ null, %then ], [ %o2, %else ]
  call void @sink(ptr addrspace(1) %p1)
  call void @sink(ptr addrspace(1) %p2)
  ret void
handler:
  %lp = landingpad i64 cleanup
  resume i64 %lp
u1:
  %lp1 = landingpad i64 cleanup
  resume i64 %lp1
u2:
  %lp2 = landingpad i64 cleanup
  resume i64 %lp2
}

!java-method-compilation = !{}

; CHECK-LABEL: define void @casea_folded_invoke_term_cascade
; The folded arraylength invoke is erased.
; CHECK-NOT: @jeandle.arraylength
; Both ORIGINAL allocation invokes are RETAINED (no fresh materialization).
; CHECK: %o1 = invoke hotspotcc ptr addrspace(1) @jeandle.new_array(ptr inttoptr (i64 12345 to ptr), i32 7, i32 44, i32 16, i32 1048576)
; CHECK: %o2 = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 22222 to ptr), i32 16)
; No pea.mat materialization invoke is emitted.
; CHECK-NOT: pea.mat = invoke
; The else block is now just a plain br to merge (no mat.cont chaining).
; CHECK: else:
; CHECK-NEXT: br label %merge
; The merge PHIs and sink calls are preserved.
; CHECK: %p1 = phi ptr addrspace(1) [ null, %then ], [ %o1, %else ]
; CHECK: %p2 = phi ptr addrspace(1) [ null, %then ], [ %o2, %else ]
; CHECK: call void @sink(ptr addrspace(1) %p1)
; CHECK: call void @sink(ptr addrspace(1) %p2)
