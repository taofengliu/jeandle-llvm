; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" -jeandle-vm-callback-log=%S/Inputs/773_copy_fold_concrete_src.cblog %s | FileCheck %s --check-prefix=KEEP
; RUN: opt -S -jeandle-pea-copy-fold-max-count=16 -passes="require<partial-escape-analysis>,partial-escape-transform" -jeandle-vm-callback-log=%S/Inputs/773_copy_fold_concrete_src.cblog %s | FileCheck %s --check-prefix=EXPAND

; Concrete sources above the default cap keep bulk copying; an explicit
; larger cap provides a positive control for the same input.

declare hotspotcc ptr addrspace(1) @jeandle.new_array(ptr, i32, i32, i32, i32)
declare void @jeandle.copy(ptr addrspace(1), i64, ptr addrspace(1), i64, i64, ptr, ptr, i64, i64)
declare i32 @__gxx_personality_v0(...)

define i32 @copy_fold_concrete_count_cap(ptr addrspace(1) "java-klass"="12345" %src) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %dest = invoke hotspotcc ptr addrspace(1) @jeandle.new_array(
            ptr inttoptr (i64 12345 to ptr), i32 16, i32 80, i32 16, i32 1048576)
         to label %do_copy unwind label %u
do_copy:
  invoke void @jeandle.copy(ptr addrspace(1) %src, i64 0, ptr addrspace(1) %dest, i64 0, i64 16,
                            ptr null, ptr null, i64 16, i64 16)
      #0 [ "deopt"(i32 99, i32 99, i64 12, ptr addrspace(1) null) ]
      to label %read unwind label %u
read:
  %db = getelementptr inbounds i8, ptr addrspace(1) %dest, i32 16
  %d0 = getelementptr inbounds i32, ptr addrspace(1) %db, i64 0
  %d1 = getelementptr inbounds i32, ptr addrspace(1) %db, i64 1
  %d2 = getelementptr inbounds i32, ptr addrspace(1) %db, i64 2
  %d3 = getelementptr inbounds i32, ptr addrspace(1) %db, i64 3
  %v0 = load atomic i32, ptr addrspace(1) %d0 unordered, align 4
  %v1 = load atomic i32, ptr addrspace(1) %d1 unordered, align 4
  %v2 = load atomic i32, ptr addrspace(1) %d2 unordered, align 4
  %v3 = load atomic i32, ptr addrspace(1) %d3 unordered, align 4
  %a1 = add i32 %v0, %v1
  %a2 = add i32 %a1, %v2
  %r = add i32 %a2, %v3
  ret i32 %r
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

attributes #0 = { "jeandle.copy.kind"="arraycopy" "jeandle.copy.validated" }

; KEEP-LABEL: define i32 @copy_fold_concrete_count_cap
; KEEP: @jeandle.new_array
; KEEP: invoke void @jeandle.copy
; KEEP: load atomic i32
; EXPAND-LABEL: define i32 @copy_fold_concrete_count_cap
; EXPAND-NOT: @jeandle.new_array
; EXPAND-NOT: @jeandle.copy
; EXPAND: %pea.copy.elem.addr = getelementptr
; EXPAND: %pea.copy.elem = load atomic i32
; EXPAND: ret i32
!java-method-compilation = !{}
