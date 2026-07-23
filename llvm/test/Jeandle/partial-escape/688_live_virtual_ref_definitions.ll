; RUN: opt -S -verify-each \
; RUN:   -passes="require<partial-escape-analysis>,partial-escape-transform" \
; RUN:   -jeandle-vm-callback-log=%S/Inputs/688_live_virtual_ref_definitions.cblog \
; RUN:   %s | FileCheck %s

; A virtual-reference dependency is a property of the current field
; definition, not of every store that ever reached the analyzer.  Each
; function below makes the outer object ineligible with HotSpot's
; value-based-class check after overwriting the child reference.  The outer
; allocation and the live null store must remain, but the overwritten
; reference store and its unreachable child allocation must still disappear.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc i1 @jeandle.check_if_value_based(ptr addrspace(1))
declare void @use_bool(i1)
declare void @may_throw()
declare void @safepoint()
declare i32 @__gxx_personality_v0(...)

define void @same_block_overwrite() gc "hotspotgc"
    personality ptr @__gxx_personality_v0 {
entry:
  %child = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
      ptr inttoptr (i64 68801 to ptr), i32 16)
      to label %alloc.outer unwind label %unwind
alloc.outer:
  %outer = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
      ptr inttoptr (i64 68802 to ptr), i32 24)
      to label %body unwind label %unwind
body:
  %slot = getelementptr inbounds i8, ptr addrspace(1) %outer, i64 16
  store atomic ptr addrspace(1) %child, ptr addrspace(1) %slot unordered, align 8
  store atomic ptr addrspace(1) null, ptr addrspace(1) %slot unordered, align 8
  %is.vb = call hotspotcc i1 @jeandle.check_if_value_based(
      ptr addrspace(1) %outer)
  call void @use_bool(i1 %is.vb)
  ret void
unwind:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @same_block_overwrite(
; CHECK-NOT: inttoptr (i64 68801 to ptr)
; CHECK: inttoptr (i64 68802 to ptr)
; CHECK-NOT: store atomic ptr addrspace(1) %child
; CHECK: store atomic ptr addrspace(1) null
; CHECK: call hotspotcc i1 @jeandle.check_if_value_based

define void @diamond_overwrite(i1 %choose) gc "hotspotgc"
    personality ptr @__gxx_personality_v0 {
entry:
  %child = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
      ptr inttoptr (i64 68811 to ptr), i32 16)
      to label %alloc.outer unwind label %unwind
alloc.outer:
  %outer = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
      ptr inttoptr (i64 68812 to ptr), i32 24)
      to label %choose.block unwind label %unwind
choose.block:
  %slot = getelementptr inbounds i8, ptr addrspace(1) %outer, i64 16
  br i1 %choose, label %left, label %right
left:
  store atomic ptr addrspace(1) %child, ptr addrspace(1) %slot unordered, align 8
  br label %merge
right:
  store atomic ptr addrspace(1) %child, ptr addrspace(1) %slot unordered, align 8
  br label %merge
merge:
  store atomic ptr addrspace(1) null, ptr addrspace(1) %slot unordered, align 8
  %is.vb = call hotspotcc i1 @jeandle.check_if_value_based(
      ptr addrspace(1) %outer)
  call void @use_bool(i1 %is.vb)
  ret void
unwind:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @diamond_overwrite(
; CHECK-NOT: inttoptr (i64 68811 to ptr)
; CHECK: inttoptr (i64 68812 to ptr)
; CHECK-NOT: store atomic ptr addrspace(1) %child
; CHECK: store atomic ptr addrspace(1) null
; CHECK: call hotspotcc i1 @jeandle.check_if_value_based

define void @loop_overwrite(i32 %count) gc "hotspotgc"
    personality ptr @__gxx_personality_v0 {
entry:
  %child = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
      ptr inttoptr (i64 68821 to ptr), i32 16)
      to label %alloc.outer unwind label %unwind
alloc.outer:
  %outer = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
      ptr inttoptr (i64 68822 to ptr), i32 24)
      to label %preheader unwind label %unwind
preheader:
  %slot = getelementptr inbounds i8, ptr addrspace(1) %outer, i64 16
  br label %header
header:
  %i = phi i32 [ 0, %preheader ], [ %next, %body ]
  %more = icmp slt i32 %i, %count
  br i1 %more, label %body, label %exit
body:
  store atomic ptr addrspace(1) %child, ptr addrspace(1) %slot unordered, align 8
  store atomic ptr addrspace(1) null, ptr addrspace(1) %slot unordered, align 8
  %next = add nuw i32 %i, 1
  br label %header
exit:
  %is.vb = call hotspotcc i1 @jeandle.check_if_value_based(
      ptr addrspace(1) %outer)
  call void @use_bool(i1 %is.vb)
  ret void
unwind:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @loop_overwrite(
; CHECK-NOT: inttoptr (i64 68821 to ptr)
; CHECK: inttoptr (i64 68822 to ptr)
; CHECK-NOT: store atomic ptr addrspace(1) %child
; CHECK: store atomic ptr addrspace(1) null
; CHECK: call hotspotcc i1 @jeandle.check_if_value_based

define void @nested_overwrite() gc "hotspotgc"
    personality ptr @__gxx_personality_v0 {
entry:
  %leaf = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
      ptr inttoptr (i64 68831 to ptr), i32 16)
      to label %alloc.middle unwind label %unwind
alloc.middle:
  %middle = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
      ptr inttoptr (i64 68832 to ptr), i32 24)
      to label %alloc.outer unwind label %unwind
alloc.outer:
  %outer = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
      ptr inttoptr (i64 68833 to ptr), i32 24)
      to label %body unwind label %unwind
body:
  %middle.slot = getelementptr inbounds i8, ptr addrspace(1) %middle, i64 16
  store atomic ptr addrspace(1) %leaf, ptr addrspace(1) %middle.slot unordered, align 8
  %outer.slot = getelementptr inbounds i8, ptr addrspace(1) %outer, i64 16
  store atomic ptr addrspace(1) %middle, ptr addrspace(1) %outer.slot unordered, align 8
  store atomic ptr addrspace(1) null, ptr addrspace(1) %outer.slot unordered, align 8
  %is.vb = call hotspotcc i1 @jeandle.check_if_value_based(
      ptr addrspace(1) %outer)
  call void @use_bool(i1 %is.vb)
  ret void
unwind:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @nested_overwrite(
; CHECK-NOT: inttoptr (i64 68831 to ptr)
; CHECK-NOT: inttoptr (i64 68832 to ptr)
; CHECK: inttoptr (i64 68833 to ptr)
; CHECK-NOT: store atomic ptr addrspace(1) %leaf
; CHECK-NOT: store atomic ptr addrspace(1) %middle
; CHECK: store atomic ptr addrspace(1) null
; CHECK: call hotspotcc i1 @jeandle.check_if_value_based

define void @deopt_observes_pre_overwrite_definition() gc "hotspotgc"
    personality ptr @__gxx_personality_v0 {
entry:
  %child = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
      ptr inttoptr (i64 68841 to ptr), i32 16)
      to label %alloc.outer unwind label %unwind
alloc.outer:
  %outer = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
      ptr inttoptr (i64 68842 to ptr), i32 24)
      to label %body unwind label %unwind
body:
  %slot = getelementptr inbounds i8, ptr addrspace(1) %outer, i64 16
  store atomic ptr addrspace(1) %child, ptr addrspace(1) %slot unordered, align 8
  call void @safepoint()
      [ "deopt"(i32 41, i32 41, i64 12, ptr addrspace(1) %outer) ]
  store atomic ptr addrspace(1) null, ptr addrspace(1) %slot unordered, align 8
  %is.vb = call hotspotcc i1 @jeandle.check_if_value_based(
      ptr addrspace(1) %outer)
  call void @use_bool(i1 %is.vb)
  ret void
unwind:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; The earlier safepoint observes outer.child, so abandoning the outer
; descriptor must retain both original allocations and the reaching store.
; CHECK-LABEL: define void @deopt_observes_pre_overwrite_definition(
; CHECK: inttoptr (i64 68841 to ptr)
; CHECK: inttoptr (i64 68842 to ptr)
; CHECK: store atomic ptr addrspace(1) %child
; CHECK: call void @safepoint() [ "deopt"(i32 41, i32 41, i64 12, ptr addrspace(1) %outer) ]
; CHECK-NOT: i64 524300
; CHECK: store atomic ptr addrspace(1) null

define void @invoke_paths_after_overwrite() gc "hotspotgc"
    personality ptr @__gxx_personality_v0 {
entry:
  %child = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
      ptr inttoptr (i64 68851 to ptr), i32 16)
      to label %alloc.outer unwind label %alloc.unwind
alloc.outer:
  %outer = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
      ptr inttoptr (i64 68852 to ptr), i32 24)
      to label %body unwind label %alloc.unwind
body:
  %slot = getelementptr inbounds i8, ptr addrspace(1) %outer, i64 16
  store atomic ptr addrspace(1) %child, ptr addrspace(1) %slot unordered, align 8
  store atomic ptr addrspace(1) null, ptr addrspace(1) %slot unordered, align 8
  %is.vb = call hotspotcc i1 @jeandle.check_if_value_based(
      ptr addrspace(1) %outer)
  call void @use_bool(i1 %is.vb)
  invoke void @may_throw() to label %normal unwind label %handler
normal:
  ret void
handler:
  %lp = landingpad i64 cleanup
  resume i64 %lp
alloc.unwind:
  %alloc.lp = landingpad i64 cleanup
  resume i64 %alloc.lp
}

; CHECK-LABEL: define void @invoke_paths_after_overwrite(
; CHECK-NOT: inttoptr (i64 68851 to ptr)
; CHECK: inttoptr (i64 68852 to ptr)
; CHECK-NOT: store atomic ptr addrspace(1) %child
; CHECK: store atomic ptr addrspace(1) null
; CHECK: invoke void @may_throw()
; CHECK: handler:
; CHECK: landingpad i64

define void @critical_edge_overwrite(i1 %take.source, i1 %early)
    gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %child = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
      ptr inttoptr (i64 68861 to ptr), i32 16)
      to label %alloc.outer unwind label %unwind
alloc.outer:
  %outer = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
      ptr inttoptr (i64 68862 to ptr), i32 24)
      to label %choose unwind label %unwind
choose:
  %slot = getelementptr inbounds i8, ptr addrspace(1) %outer, i64 16
  br i1 %take.source, label %source, label %right
source:
  store atomic ptr addrspace(1) %child, ptr addrspace(1) %slot unordered, align 8
  br i1 %early, label %early.exit, label %merge
right:
  store atomic ptr addrspace(1) %child, ptr addrspace(1) %slot unordered, align 8
  br label %merge
merge:
  store atomic ptr addrspace(1) null, ptr addrspace(1) %slot unordered, align 8
  %is.vb = call hotspotcc i1 @jeandle.check_if_value_based(
      ptr addrspace(1) %outer)
  call void @use_bool(i1 %is.vb)
  ret void
early.exit:
  ret void
unwind:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @critical_edge_overwrite(
; CHECK-NOT: inttoptr (i64 68861 to ptr)
; CHECK: inttoptr (i64 68862 to ptr)
; CHECK-NOT: store atomic ptr addrspace(1) %child
; CHECK: store atomic ptr addrspace(1) null
; CHECK: call hotspotcc i1 @jeandle.check_if_value_based
; CHECK: early.exit:
; CHECK-NEXT: ret void

!java-method-compilation = !{}
