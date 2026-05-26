; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck --check-prefix=DEFAULT %s
; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" -jeandle-pea-max-array-length=32 %s | FileCheck --check-prefix=LOW %s
; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" -jeandle-pea-max-array-length=128 %s | FileCheck --check-prefix=HIGH %s

; jeandle-pea-max-array-length default is 128 (Graal parity).
; This test pins both a 33-element array (between the old 32 default and
; the new 128 default) and a 129-element array (always above the cap)
; into the same IR so the cap can be exercised by varying the cl::opt:
;
;   * DEFAULT (no flag passed): cap is 128. The 33-elem array virtualizes
;     and folds to its constant length; the 129-elem array bypasses PEA.
;   * LOW (-jeandle-pea-max-array-length=32): cap is 32. Both arrays
;     bypass PEA — both jeandle.newarray invokes survive.
;   * HIGH (-jeandle-pea-max-array-length=128): same observable effect
;     as DEFAULT (the flag is set explicitly to the default).
;
; Folding manifests as: jeandle.newarray gone + jeandle.array_length
; replaced with the constant length integer.

declare hotspotcc ptr addrspace(1) @jeandle.newarray(ptr, i32)
declare hotspotcc i32 @jeandle.array_length(ptr addrspace(1) readonly)
declare i32 @__gxx_personality_v0(...)

define i32 @test_len33() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %arr = invoke hotspotcc ptr addrspace(1) @jeandle.newarray(
            ptr inttoptr (i64 12345 to ptr), i32 33)
         to label %n unwind label %u
n:
  %len = call hotspotcc i32 @jeandle.array_length(ptr addrspace(1) %arr)
  ret i32 %len
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

define i32 @test_len129() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %arr = invoke hotspotcc ptr addrspace(1) @jeandle.newarray(
            ptr inttoptr (i64 12345 to ptr), i32 129)
         to label %n unwind label %u
n:
  %len = call hotspotcc i32 @jeandle.array_length(ptr addrspace(1) %arr)
  ret i32 %len
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; DEFAULT (cap=128): 33 virtualized, 129 bypassed.
; DEFAULT-LABEL: define i32 @test_len33
; DEFAULT-NOT: jeandle.newarray
; DEFAULT-NOT: jeandle.array_length
; DEFAULT: ret i32 33

; DEFAULT-LABEL: define i32 @test_len129
; DEFAULT: invoke{{.*}}@jeandle.newarray{{.*}}i32 129
; DEFAULT: call{{.*}}@jeandle.array_length

; LOW (cap=32): both bypassed.
; LOW-LABEL: define i32 @test_len33
; LOW: invoke{{.*}}@jeandle.newarray{{.*}}i32 33
; LOW: call{{.*}}@jeandle.array_length

; LOW-LABEL: define i32 @test_len129
; LOW: invoke{{.*}}@jeandle.newarray{{.*}}i32 129
; LOW: call{{.*}}@jeandle.array_length

; HIGH (cap=128): same as DEFAULT.
; HIGH-LABEL: define i32 @test_len33
; HIGH-NOT: jeandle.newarray
; HIGH-NOT: jeandle.array_length
; HIGH: ret i32 33

; HIGH-LABEL: define i32 @test_len129
; HIGH: invoke{{.*}}@jeandle.newarray{{.*}}i32 129
; HIGH: call{{.*}}@jeandle.array_length

!java-method-compilation = !{}
