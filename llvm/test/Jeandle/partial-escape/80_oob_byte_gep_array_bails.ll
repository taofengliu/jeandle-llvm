; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" -jeandle-vm-callback-log=%S/Inputs/80_oob_byte_gep_array_bails.cblog %s | FileCheck %s

; int[3] virtual array (ArrayBaseOffset=16, index scale 4). A byte-GEP at
; offset 28 = 16 + 3*4 is ONE PAST THE END (index 3, length 3) — out of
; bounds. matchArrayElementGEP (Pattern B) correctly rejects it, but before
; the fix the generic constant-offset resolver then ACCEPTED 28 as a phantom
; field (it only checked the lower bound `Offset < ArrayBaseOffset`). The
; phantom field was never replayed at materialization (the emit loop walks
; only 0..ArrayLength-1), so a store to it was silently lost and a
; NeverEscapes array could be eliminated while a live store targeted memory
; past its end. The fix adds the upper-bound check `Offset >= ArrayBaseOffset
; + ArrayLength*scale -> bail`, so the array is kept real and the store
; survives.

declare hotspotcc ptr addrspace(1) @jeandle.new_array(ptr, i32, i32, i32, i32)

declare i32 @__gxx_personality_v0(...)

define void @test_oob_byte_gep_array_bails() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  ; length 3, ArrayBaseOffset 16, Int element (scale 4) via the cblog.
  %arr = invoke hotspotcc ptr addrspace(1) @jeandle.new_array(
            ptr inttoptr (i64 12345 to ptr), i32 3, i32 32, i32 16, i32 1048576)
       to label %n unwind label %u
n:
  ; In-bounds element 0 store, so the array is otherwise virtualizable.
  %base = getelementptr inbounds i8, ptr addrspace(1) %arr, i32 16
  %p0 = getelementptr inbounds i32, ptr addrspace(1) %base, i64 0
  store atomic i32 10, ptr addrspace(1) %p0 unordered, align 4
  ; OOB byte-GEP: offset 28 = 16 + 3*4 (index 3, one past the length-3 end).
  %bad = getelementptr inbounds i8, ptr addrspace(1) %arr, i64 28
  store atomic i32 99, ptr addrspace(1) %bad unordered, align 4
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; The OOB access bails the array (markIneligible -> kept real): the alloc and
; BOTH stores survive. Before the fix the OOB offset was modeled as a phantom
; field, the array was eliminated, and the stores vanished.
; CHECK-LABEL: define void @test_oob_byte_gep_array_bails
; CHECK: jeandle.new_array
; CHECK: store atomic i32 10
; CHECK: store atomic i32 99
; CHECK: ret void

!java-method-compilation = !{}
