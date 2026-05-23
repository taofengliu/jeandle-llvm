; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" -jeandle-vm-callback-log=%S/Inputs/381_unsafe_put_int_into_byte_array.cblog %s | FileCheck %s

; R11.V12: Unsafe.putInt-style decomposition. A virtual byte[] is written
; via a single i32 store at the array's base offset; PEA decomposes the
; i32 value (0x1234ABCD = 305441741) into 4 byte FieldStates entries
; little-endian. Read-back of byte 2 (offset+2) returns 0x34 = 52.

declare hotspotcc ptr addrspace(1) @jeandle.newarray(ptr, i32)
declare i32 @__gxx_personality_v0(...)

define i8 @t() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %arr = invoke hotspotcc ptr addrspace(1) @jeandle.newarray(
            ptr inttoptr (i64 33333 to ptr), i32 8)
         to label %n unwind label %u
n:
  %base = getelementptr inbounds i8, ptr addrspace(1) %arr, i32 16
  ; Wide i32 store onto the byte array — V12 decomposes into 4 i8 entries.
  store atomic i32 305441741, ptr addrspace(1) %base unordered, align 1
  ; Read byte 2 (LE byte 2 of 0x1234ABCD = 0x34).
  %p2 = getelementptr inbounds i8, ptr addrspace(1) %base, i64 2
  %v = load atomic i8, ptr addrspace(1) %p2 unordered, align 1
  ret i8 %v
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define i8 @t
; CHECK-NOT: jeandle.newarray
; CHECK-NOT: store
; CHECK-NOT: load
; CHECK: ret i8 52

!java-method-compilation = !{}
