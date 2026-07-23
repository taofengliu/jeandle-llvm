; RUN: opt -S -verify-each \
; RUN:   -passes="require<partial-escape-analysis>,partial-escape-transform" \
; RUN:   -jeandle-vm-callback-log=%S/Inputs/689_array_deopt_noncanonical_cells.cblog \
; RUN:   %s | FileCheck %s

; A virtual-array descriptor represents exactly one canonical cell per Java
; element.  Any touched byte cell that cannot be mapped to
; base + index*scale with the array's exact element type must reject the whole
; descriptor.  The fallback reuses the original allocation, replays the
; tracked store before the safepoint, and leaves the real oop in the bundle.

declare hotspotcc ptr addrspace(1) @jeandle.new_array(ptr, i32, i32, i32, i32)
declare void @safepoint()
declare i32 @__gxx_personality_v0(...)

define void @noncanonical_byte_cell() gc "hotspotgc"
    personality ptr @__gxx_personality_v0 {
entry:
  %arr = invoke hotspotcc ptr addrspace(1) @jeandle.new_array(
      ptr inttoptr (i64 68901 to ptr), i32 2, i32 24, i32 16, i32 1048576)
      to label %body unwind label %unwind
body:
  %base = getelementptr inbounds i8, ptr addrspace(1) %arr, i64 16
  %partial = getelementptr inbounds i8, ptr addrspace(1) %base, i64 1
  store atomic i8 7, ptr addrspace(1) %partial unordered, align 1
  call void @safepoint()
      [ "deopt"(i32 1, i32 1, i64 12, ptr addrspace(1) %arr) ]
  ret void
unwind:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @noncanonical_byte_cell(
; CHECK-COUNT-1: invoke hotspotcc ptr addrspace(1) @jeandle.new_array
; CHECK: %[[SLOT:[-A-Za-z$._0-9]+]] = getelementptr inbounds i8, ptr addrspace(1) %arr, i64 17
; CHECK-NEXT: store atomic i8 7, ptr addrspace(1) %[[SLOT]] unordered, align 1
; CHECK-NEXT: call void @safepoint() [ "deopt"(i32 1, i32 1, i64 12, ptr addrspace(1) %arr) ]
; CHECK-NOT: i64 262157
; CHECK-NOT: i64 524300

define void @partial_canonical_cell() gc "hotspotgc"
    personality ptr @__gxx_personality_v0 {
entry:
  %arr = invoke hotspotcc ptr addrspace(1) @jeandle.new_array(
      ptr inttoptr (i64 68902 to ptr), i32 2, i32 24, i32 16, i32 1048576)
      to label %body unwind label %unwind
body:
  %base = getelementptr inbounds i8, ptr addrspace(1) %arr, i64 16
  store atomic i16 4660, ptr addrspace(1) %base unordered, align 2
  call void @safepoint()
      [ "deopt"(i32 2, i32 2, i64 12, ptr addrspace(1) %arr) ]
  ret void
unwind:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @partial_canonical_cell(
; CHECK-COUNT-1: invoke hotspotcc ptr addrspace(1) @jeandle.new_array
; CHECK: %[[SLOT:[-A-Za-z$._0-9]+]] = getelementptr inbounds i8, ptr addrspace(1) %arr, i64 16
; CHECK-NEXT: store atomic i16 4660, ptr addrspace(1) %[[SLOT]] unordered, align 2
; CHECK-NEXT: call void @safepoint() [ "deopt"(i32 2, i32 2, i64 12, ptr addrspace(1) %arr) ]
; CHECK-NOT: i64 262157
; CHECK-NOT: i64 524300

define void @wrong_kind_same_width() gc "hotspotgc"
    personality ptr @__gxx_personality_v0 {
entry:
  %arr = invoke hotspotcc ptr addrspace(1) @jeandle.new_array(
      ptr inttoptr (i64 68903 to ptr), i32 2, i32 24, i32 16, i32 1048576)
      to label %body unwind label %unwind
body:
  %base = getelementptr inbounds i8, ptr addrspace(1) %arr, i64 16
  store atomic float 1.500000e+00, ptr addrspace(1) %base unordered, align 4
  call void @safepoint()
      [ "deopt"(i32 3, i32 3, i64 12, ptr addrspace(1) %arr) ]
  ret void
unwind:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @wrong_kind_same_width(
; CHECK-COUNT-1: invoke hotspotcc ptr addrspace(1) @jeandle.new_array
; CHECK: %[[SLOT:[-A-Za-z$._0-9]+]] = getelementptr inbounds i8, ptr addrspace(1) %arr, i64 16
; CHECK-NEXT: store atomic float 1.500000e+00, ptr addrspace(1) %[[SLOT]] unordered, align 4
; CHECK-NEXT: call void @safepoint() [ "deopt"(i32 3, i32 3, i64 12, ptr addrspace(1) %arr) ]
; CHECK-NOT: i64 262157
; CHECK-NOT: i64 524300

define void @zero_offset_wrappers_are_canonical() gc "hotspotgc"
    personality ptr @__gxx_personality_v0 {
entry:
  %arr = invoke hotspotcc ptr addrspace(1) @jeandle.new_array(
      ptr inttoptr (i64 68904 to ptr), i32 2, i32 24, i32 16, i32 1048576)
      to label %body unwind label %unwind
body:
  %identity = getelementptr inbounds i8, ptr addrspace(1) %arr, i64 0
  %frozen = freeze ptr addrspace(1) %identity
  %base = getelementptr inbounds i8, ptr addrspace(1) %frozen, i64 16
  %cell = getelementptr inbounds i32, ptr addrspace(1) %base, i64 1
  store atomic i32 99, ptr addrspace(1) %cell unordered, align 4
  call void @safepoint()
      [ "deopt"(i32 4, i32 4, i64 12, ptr addrspace(1) %arr) ]
  ret void
unwind:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @zero_offset_wrappers_are_canonical(
; CHECK-NOT: @jeandle.new_array
; CHECK: call void @safepoint()
; CHECK-SAME: [ "deopt"(i32 4, i32 4,
; CHECK-SAME: i64 262157, i64 68904, i32 2,
; CHECK-SAME: i64 68719476746, i32 0,
; CHECK-SAME: i64 85899345930, i32 99,
; CHECK-SAME: i64 524300, i32 0) ]

define void @symbolic_offset_materializes_before_deopt(i64 %index)
    gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %arr = invoke hotspotcc ptr addrspace(1) @jeandle.new_array(
      ptr inttoptr (i64 68905 to ptr), i32 2, i32 24, i32 16, i32 1048576)
      to label %body unwind label %unwind
body:
  %base = getelementptr inbounds i8, ptr addrspace(1) %arr, i64 16
  %cell = getelementptr inbounds i32, ptr addrspace(1) %base, i64 %index
  store atomic i32 77, ptr addrspace(1) %cell unordered, align 4
  call void @safepoint()
      [ "deopt"(i32 5, i32 5, i64 12, ptr addrspace(1) %arr) ]
  ret void
unwind:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @symbolic_offset_materializes_before_deopt(
; CHECK-COUNT-1: invoke hotspotcc ptr addrspace(1) @jeandle.new_array
; CHECK: store atomic i32 77, ptr addrspace(1) %cell unordered, align 4
; CHECK-NEXT: call void @safepoint() [ "deopt"(i32 5, i32 5, i64 12, ptr addrspace(1) %arr) ]
; CHECK-NOT: i64 262157
; CHECK-NOT: i64 524300

!java-method-compilation = !{}
