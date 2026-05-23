; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; R6.S8 — updateStatesForMaterialized for the outer VO.
;
; Two virtual objects: A (outer) and B (sibling) where B holds a field that
; stores a reference to A (FS[B][0] = VirtualRef(A)). When A escapes via
; @sink BEFORE B does, materializeAt(A) flips A to Materialized but the
; sibling B's FieldStates entry still names VirtualRef(A). Later, when B
; itself escapes, the snapshotting loop would either fire the transform-side
; debug assert ("VirtualRef field entries must have been rewritten to
; MaterializedRef during analysis") or silently drop B's field-0 store.
;
; Post-fix: after A's outer state flips to Materialized, the helper walks
; every other VO's FieldStates and rewrites VirtualRef(A) entries to
; MaterializedRef(A.alloc). When B subsequently escapes, its snapshot has
; a clean MaterializedRef entry and the materialised invoke for B replays
; the field-0 store with A's materialised pointer.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @sibling_ref() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %a = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 11111 to ptr), i32 16)
       to label %n1 unwind label %u
n1:
  %b = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 22222 to ptr), i32 24)
       to label %n2 unwind label %u
n2:
  ; B.field_0 = A — gives FS[B][0] = VirtualRef(A).
  %bf0 = getelementptr inbounds i8, ptr addrspace(1) %b, i64 0
  store atomic ptr addrspace(1) %a, ptr addrspace(1) %bf0 unordered, align 8
  ; Escape A first.
  call void @sink(ptr addrspace(1) %a)
  ; Now escape B. Before R6.S8 this would assert or drop the field.
  call void @sink(ptr addrspace(1) %b)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; Both A and B are materialised; B's field-0 store names A's materialised
; pointer rather than the original (now-erased) allocation.
; CHECK-LABEL: define void @sibling_ref
; CHECK: %[[A:[A-Za-z0-9._]+]] = invoke {{.*}}@jeandle.new_instance(ptr inttoptr (i64 11111 to ptr)
; CHECK: %[[B:[A-Za-z0-9._]+]] = invoke {{.*}}@jeandle.new_instance(ptr inttoptr (i64 22222 to ptr)
; CHECK: store atomic ptr addrspace(1) %[[A]], ptr addrspace(1) %{{.*}}

!java-method-compilation = !{}
