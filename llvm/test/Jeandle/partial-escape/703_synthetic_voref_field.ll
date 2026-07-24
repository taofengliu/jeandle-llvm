; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s --check-prefix=IR

; A Case-C synthetic VO whose field holds ANOTHER virtual object (the same
; %inner stored into both branches, so AllSame across preds) is DESCRIBED with a
; VORef FIELD pointing at %inner's descriptor. %inner is reached only
; transitively (not a direct bundle operand), so the transitive descriptor
; closure must describe it. Mirrors C2/Graal nested ObjectValue + id back-ref.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32) nounwind
declare void @safepoint()

define void @synthetic_with_inner_field(i1 %c) gc "hotspotgc" {
entry:
  %inner = call hotspotcc ptr addrspace(1) @jeandle.new_instance(
      ptr inttoptr (i64 70302 to ptr), i32 16) [ "deopt"(i32 703001) ]
  %innerf = getelementptr inbounds i8, ptr addrspace(1) %inner, i64 8
  store atomic i32 77, ptr addrspace(1) %innerf unordered, align 4
  br i1 %c, label %left, label %right
left:
  %a = call hotspotcc ptr addrspace(1) @jeandle.new_instance(
      ptr inttoptr (i64 70301 to ptr), i32 24) [ "deopt"(i32 703011) ]
  %af = getelementptr inbounds i8, ptr addrspace(1) %a, i64 8
  store atomic ptr addrspace(1) %inner, ptr addrspace(1) %af unordered, align 8
  br label %merge
right:
  %b = call hotspotcc ptr addrspace(1) @jeandle.new_instance(
      ptr inttoptr (i64 70301 to ptr), i32 24) [ "deopt"(i32 703012) ]
  %bf = getelementptr inbounds i8, ptr addrspace(1) %b, i64 8
  store atomic ptr addrspace(1) %inner, ptr addrspace(1) %bf unordered, align 8
  br label %merge
merge:
  %p = phi ptr addrspace(1) [ %a, %left ], [ %b, %right ]
  call void @safepoint()
      [ "deopt"(i32 99, i32 99, i64 12, ptr addrspace(1) %p) ]
  ret void
}

; IR-LABEL: define void @synthetic_with_inner_field(
; All three allocations eliminated; no replay.
; IR-NOT: call {{.*}}@jeandle.new_instance
; IR-NOT: store atomic
; IR: call void @safepoint() [ "deopt"(
; IR-SAME: i32 99, i32 99,
; %inner descriptor (vo_id=0): klass 70302, field_count 1, offset 8 = 77.
; IR-SAME: i64 262156, i64 70302, i32 1,
; IR-SAME: i64 34359738378, i32 77,
; synthetic %p descriptor (vo_id=3): klass 70301, field_count 1, field offset 8
; = VORef to %inner (vo_id=0): (8<<32)|(8<<16)|12 = 34360262668.
; IR-SAME: i64 12885164044, i64 70301, i32 1,
; IR-SAME: i64 34360262668, i32 0,
; %p slot -> VORef vo_id=3.
; IR-SAME: i64 12885426188, i32 3) ]
; IR-NOT: poison

!java-method-compilation = !{}
