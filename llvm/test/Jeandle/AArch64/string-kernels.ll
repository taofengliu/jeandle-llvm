; RUN: opt -passes='default<O3>' -S %s | llc -mtriple=aarch64 -mattr=+neon | FileCheck %s
;
; The hot equality test must not build a lane mask. Recovering the first
; mismatching lane belongs on the unequal edge in the String template.

declare i8 @llvm.vector.reduce.umax.v16i8(<16 x i8>)

define i1 @equal32(ptr %a, ptr %b) {
  %av = load <32 x i8>, ptr %a, align 1
  %bv = load <32 x i8>, ptr %b, align 1
  %xor = xor <32 x i8> %av, %bv
  %lo = shufflevector <32 x i8> %xor, <32 x i8> poison, <16 x i32> <i32 0, i32 1, i32 2, i32 3, i32 4, i32 5, i32 6, i32 7, i32 8, i32 9, i32 10, i32 11, i32 12, i32 13, i32 14, i32 15>
  %hi = shufflevector <32 x i8> %xor, <32 x i8> poison, <16 x i32> <i32 16, i32 17, i32 18, i32 19, i32 20, i32 21, i32 22, i32 23, i32 24, i32 25, i32 26, i32 27, i32 28, i32 29, i32 30, i32 31>
  %or = or <16 x i8> %lo, %hi
  %max = call i8 @llvm.vector.reduce.umax.v16i8(<16 x i8> %or)
  %eq = icmp eq i8 %max, 0
  ret i1 %eq
}
; CHECK-LABEL: equal32:
; CHECK-NOT: zip1
; CHECK-NOT: addv
; CHECK: eor
; CHECK: eor
; CHECK: orr
; CHECK: umaxv
; CHECK-NOT: zip1
; CHECK-NOT: addv
; CHECK: ret

; Confirm two Latin1 characters as native UTF16 lanes with one exact load.
; In particular, there must be no SIMD-to-stack-to-integer round trip.
define i32 @packed_latin_pair(ptr %data) {
  %pair = load i16, ptr %data, align 1
  %value = zext i16 %pair to i32
  %shifted = shl i32 %value, 8
  %spread = or i32 %value, %shifted
  %result = and i32 %spread, 16711935
  ret i32 %result
}
; CHECK-LABEL: packed_latin_pair:
; CHECK-NOT: sp
; CHECK: ldrh
; CHECK-NOT: sp
; CHECK: orr
; CHECK-NOT: sp
; CHECK: and
; CHECK-NOT: sp
; CHECK: ret
