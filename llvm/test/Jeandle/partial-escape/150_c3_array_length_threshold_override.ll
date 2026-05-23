; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s --check-prefix=DEFAULT
; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" -jeandle-pea-max-array-length=8 %s | FileCheck %s --check-prefix=OVERRIDE

; PEA: the MaximumEscapeAnalysisArrayLength cap is exposed as the
; -jeandle-pea-max-array-length cl::opt. An array of length 9 is below
; the default cap of 32, so the allocation virtualizes and a follow-up
; jeandle.array_length folds to a constant. With -jeandle-pea-max-array-length=8
; the same array exceeds the cap, tier1Allocate refuses to register a
; virtual, and both the allocation and the array_length call survive.

declare hotspotcc ptr addrspace(1) @jeandle.newarray(ptr, i32)
declare hotspotcc i32 @jeandle.array_length(ptr addrspace(1) readonly)

declare i32 @__gxx_personality_v0(...)

define i32 @test_len9() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %arr = invoke hotspotcc ptr addrspace(1) @jeandle.newarray(
            ptr inttoptr (i64 12345 to ptr), i32 9)
         to label %n unwind label %u
n:
  %len = call hotspotcc i32 @jeandle.array_length(ptr addrspace(1) %arr)
  ret i32 %len
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; Default cap (32): length-9 array virtualizes, allocation & array_length fold away.
; DEFAULT-LABEL: define i32 @test_len9
; DEFAULT-NOT: jeandle.newarray
; DEFAULT-NOT: jeandle.array_length
; DEFAULT: ret i32 9

; Override cap to 8: length-9 array exceeds the cap, both the allocation
; and the array_length call survive in IR.
; OVERRIDE-LABEL: define i32 @test_len9
; OVERRIDE: jeandle.newarray
; OVERRIDE: jeandle.array_length

!java-method-compilation = !{}
