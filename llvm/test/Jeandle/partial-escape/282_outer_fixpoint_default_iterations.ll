; RUN: opt -S -passes="partial-escape-iterative" %s \
; RUN:   | FileCheck %s --check-prefix=DEFAULT
; RUN: opt -S -passes="partial-escape-iterative" -jeandle-pea-iterations=1 %s \
; RUN:   | FileCheck %s --check-prefix=ONE

; The wrapper's default iteration cap is 2. This test pins both sides
; of that contract:
;
;   DEFAULT prefix  -- no `-jeandle-pea-iterations` flag, so the cl::opt
;                      default (now 2) applies. Same input as 280; we
;                      expect full elimination after round 1's
;                      materialization is canonicalized away by
;                      InstCombine+SimplifyCFG+ADCE and round 2
;                      re-virtualizes the freshly-emitted alloc.
;
;   ONE prefix      -- `-jeandle-pea-iterations=1` opt-down for callers
;                      that want strict single-round semantics. With the
;                      cap at 1 the wrapper still does one
;                      analyze+transform pair; round 1's materialization
;                      survives because no canonicalization runs between
;                      rounds and no round 2 happens.
;
; If we ever bump the default again, the DEFAULT side of this test moves
; with it; the ONE side stays constant.

@G_zero = private unnamed_addr constant i32 0

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define i32 @test_default_two_rounds()
    gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %n unwind label %u
n:
  %v = load i32, ptr @G_zero, align 4
  %c = icmp ne i32 %v, 0
  br i1 %c, label %escape, label %fast
escape:
  call void @sink(ptr addrspace(1) %o)
  br label %fast
fast:
  %slot = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  store atomic i32 42, ptr addrspace(1) %slot unordered, align 4
  %val = load atomic i32, ptr addrspace(1) %slot unordered, align 4
  ret i32 %val
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; DEFAULT-LABEL: define i32 @test_default_two_rounds()
; DEFAULT-NOT: jeandle.new_instance
; DEFAULT-NOT: call void @sink
; DEFAULT: ret i32 42

; ONE-LABEL: define i32 @test_default_two_rounds()
; ONE: jeandle.new_instance
; ONE: call void @sink

!java-method-compilation = !{}
