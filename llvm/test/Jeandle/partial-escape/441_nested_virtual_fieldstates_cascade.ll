; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" -jeandle-vm-callback-log=%S/Inputs/441_nested_virtual_fieldstates_cascade.cblog %s | FileCheck %s

; Nested-virtual poison via FieldStates VirtualRef (review §1.1).
;
; An Object[] array `arr` first receives a CONSTANT-index store of a fresh
; virtual instance `inner0` (arr[0] = inner0): processStore succeeds, records
; FieldStates[arr][16] = VirtualRef(inner0) and emits an EliminateStoreEffect
; for arr. A second store at a SYMBOLIC index `%i` (arr[i] = innerI) fails
; resolveAccess -> bailKeepingOperandsReal -> markIneligible(arr) + innerI.
; commit() then dropEffectsFor(arr) erases arr's EliminateStoreEffect, so the
; arr[0] store survives as a real store. Without this fix, markIneligible
; cascades only SyntheticSourceIDs, so inner0 is left eligible with no
; surviving Materialize -> classified NeverEscapes -> Pass 2 RAUWs
; inner0.OrigAlloc to poison -> the surviving real arr[0] store writes poison
; into the escaped array -> miscompile.
;
; With the fix, markIneligible also cascades FieldStates[arr] VirtualRef
; entries (mirroring Graal's recursive entry cascade in
; PartialEscapeBlockState.java:292-343 and Jeandle's live-path
; ensureMaterialized:4693-4740), so inner0 is also marked ineligible ->
; dropEffectsFor(inner0) drops its EliminateAllocationEffect -> inner0.OrigAlloc
; survives as a real allocation -> the surviving arr[0] store writes the real
; inner0 pointer. Both inner0 and innerI stay real; no poison.

declare hotspotcc ptr addrspace(1) @jeandle.new_array(ptr, i32)
declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_nested_virtual_fieldstates_cascade(i64 %i) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %arr = invoke hotspotcc ptr addrspace(1) @jeandle.new_array(
            ptr inttoptr (i64 8888 to ptr), i32 10)
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
  ; Constant index 0 -> processStore succeeds, records VirtualRef(inner0).
  %slot0 = getelementptr inbounds ptr addrspace(1), ptr addrspace(1) %base, i64 0
  store atomic ptr addrspace(1) %inner0, ptr addrspace(1) %slot0 unordered, align 4
  ; Symbolic index %i -> resolveAccess returns nullopt -> bail ->
  ; markIneligible(arr) must cascade inner0 (FieldStates[arr][16]).
  %slotI = getelementptr inbounds ptr addrspace(1), ptr addrspace(1) %base, i64 %i
  store atomic ptr addrspace(1) %innerI, ptr addrspace(1) %slotI unordered, align 4
  call void @sink(ptr addrspace(1) %arr)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @test_nested_virtual_fieldstates_cascade
; The array allocation survives real (it escapes to @sink).
; CHECK: jeandle.new_array
; Both inner instances survive real: inner0 because markIneligible(arr)
; cascaded the FieldStates VirtualRef, innerI because it was the bailing
; store's value operand. Without the fix inner0 -> NeverEscapes -> its
; jeandle.new_instance is deleted, leaving only one.
; CHECK-COUNT-2: invoke hotspotcc{{.*}}@jeandle.new_instance
; No poison must reach the surviving real stores.
; CHECK-NOT: poison

!java-method-compilation = !{}
