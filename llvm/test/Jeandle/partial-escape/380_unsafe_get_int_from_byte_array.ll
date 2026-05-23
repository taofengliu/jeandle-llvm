; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" -jeandle-vm-callback-log=%S/Inputs/380_unsafe_get_int_from_byte_array.cblog %s | FileCheck %s

; R11.V11: Unsafe.getInt-style reassembly. A virtual byte[] is written
; byte-by-byte at offsets 16..19 (the int[]/byte[] base offset is 16
; per the cblog), then read as a wider integer at the same offset.
; PEA reassembles the integer via the byte FieldStates entries — when
; all four bytes are constant, the result folds to a ConstantInt little-
; endian.
;
; bytes: 16 -> 0xCD, 17 -> 0xAB, 18 -> 0x34, 19 -> 0x12
; reassembled int = 0x1234ABCD

declare hotspotcc ptr addrspace(1) @jeandle.newarray(ptr, i32)
declare i32 @__gxx_personality_v0(...)

define i32 @t() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %arr = invoke hotspotcc ptr addrspace(1) @jeandle.newarray(
            ptr inttoptr (i64 33333 to ptr), i32 8)
         to label %n unwind label %u
n:
  %base = getelementptr inbounds i8, ptr addrspace(1) %arr, i32 16
  %p0 = getelementptr inbounds i8, ptr addrspace(1) %base, i64 0
  %p1 = getelementptr inbounds i8, ptr addrspace(1) %base, i64 1
  %p2 = getelementptr inbounds i8, ptr addrspace(1) %base, i64 2
  %p3 = getelementptr inbounds i8, ptr addrspace(1) %base, i64 3
  store atomic i8 -51, ptr addrspace(1) %p0 unordered, align 1   ; 0xCD
  store atomic i8 -85, ptr addrspace(1) %p1 unordered, align 1   ; 0xAB
  store atomic i8 52, ptr addrspace(1) %p2 unordered, align 1    ; 0x34
  store atomic i8 18, ptr addrspace(1) %p3 unordered, align 1    ; 0x12
  ; Reassembly: load i32 at byte offset 16, straddles 4 byte slots.
  %v = load atomic i32, ptr addrspace(1) %p0 unordered, align 1
  ret i32 %v
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define i32 @t
; CHECK-NOT: jeandle.newarray
; CHECK-NOT: store
; CHECK-NOT: load
; 0x1234ABCD = 305441741
; CHECK: ret i32 305441741

!java-method-compilation = !{}
