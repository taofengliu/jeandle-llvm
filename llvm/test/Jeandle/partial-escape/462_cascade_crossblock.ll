; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" -jeandle-vm-callback-log=%S/Inputs/441_nested_virtual_fieldstates_cascade.cblog %s | FileCheck %s

; Cross-block VirtualRef cascade (review §3 #1): 441's pattern moved out of
; the LAST block. arr[0]=inner0 (constant index) records VirtualRef and an
; EliminateStore; arr[i]=innerI (SYMBOLIC index) cannot be virtualized, so
; arr and innerI materialize AT the symbolic store; the arr escape is in
; `tail`. The pre-fix commit cascade read the per-block live FieldStates —
; which held only `tail`'s (empty) state — missed the arr->inner0 edge, left
; inner0 NeverEscapes, and the surviving real store wrote POISON into the
; escaped array. The persistent VirtualRefEdges set (recorded at
; processStore) now carries the edge across blocks: inner0 is materialized
; as well, the tracked arr[0]=inner0 store is replayed onto arr's OrigAlloc
; (pea.matslot) immediately before the symbolic store, and the symbolic
; store writes the real innerI pointer.

declare hotspotcc ptr addrspace(1) @jeandle.new_array(ptr, i32, i32, i32, i32)
declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_crossblock(i64 %i, i1 %c) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %arr = invoke hotspotcc ptr addrspace(1) @jeandle.new_array(
            ptr inttoptr (i64 8888 to ptr), i32 10, i32 56, i32 16, i32 1048576)
         to label %n0 unwind label %u
n0:
  %inner0 = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 5555 to ptr), i32 24)
           to label %n1 unwind label %u
n1:
  %innerI = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 5556 to ptr), i32 24)
           to label %n2 unwind label %u
n2:
  %base = getelementptr inbounds i8, ptr addrspace(1) %arr, i32 16
  %slot0 = getelementptr inbounds ptr addrspace(1), ptr addrspace(1) %base, i64 0
  store atomic ptr addrspace(1) %inner0, ptr addrspace(1) %slot0 unordered, align 4
  %slotI = getelementptr inbounds ptr addrspace(1), ptr addrspace(1) %base, i64 %i
  store atomic ptr addrspace(1) %innerI, ptr addrspace(1) %slotI unordered, align 4
  br label %tail

tail:
  call void @sink(ptr addrspace(1) %arr)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; All three allocations retained; the symbolic store materializes arr (and
; innerI) AT the store, replaying the tracked arr[0]=inner0 store onto
; OrigAlloc (pea.matslot) immediately before it; both surviving stores write
; the real inner pointers; no poison anywhere.
; CHECK-LABEL: define void @test_crossblock(
; CHECK: invoke hotspotcc ptr addrspace(1) @jeandle.new_array
; CHECK: invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 5555 to ptr), i32 24)
; CHECK: invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 5556 to ptr), i32 24)
; CHECK: %pea.matslot = getelementptr inbounds i8, ptr addrspace(1) %arr, i64 16
; CHECK: store atomic ptr addrspace(1) %inner0, ptr addrspace(1) %pea.matslot unordered, align 8
; CHECK: store atomic ptr addrspace(1) %innerI, ptr addrspace(1) %slotI unordered, align 4
; CHECK: call void @sink(ptr addrspace(1) %arr)
; CHECK-NOT: poison

!java-method-compilation = !{}
