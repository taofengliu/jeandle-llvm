; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s --check-prefix=DEFAULT
; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" -jeandle-pea-loop-cutoff=2 %s | FileCheck %s --check-prefix=OVERRIDE

; -jeandle-pea-loop-cutoff exposes the depth threshold. Default is
; 20, so a 3-deep nest does NOT trip Mode::StopNewInLoopNest and the
; loop-local alloc at depth 3 follows normal Regular-mode handling
; (escapes via @sink so materializes; OrigAlloc invoke RAUW'd to a new
; invoke at the materialize site — i64 4444 still appears in IR but only
; on the materialised invoke).
;
; With -jeandle-pea-loop-cutoff=2, the same nest crosses the threshold,
; StopNew kicks in, processAllocation refuses the depth-3 alloc, and the
; ORIGINAL invoke (with its original SSA name) survives unchanged. Use
; the @sink to anchor a CHECK that distinguishes the two paths.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_cutoff(i32 %n) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  br label %hdr1

hdr1:
  %i1 = phi i32 [0, %entry], [%inc1, %latch1]
  %c1 = icmp slt i32 %i1, %n
  br i1 %c1, label %body1, label %exit1
body1:
  br label %hdr2
latch1:
  %inc1 = add i32 %i1, 1
  br label %hdr1

hdr2:
  %i2 = phi i32 [0, %body1], [%inc2, %latch2]
  %c2 = icmp slt i32 %i2, %n
  br i1 %c2, label %body2, label %exit2
body2:
  br label %hdr3
latch2:
  %inc2 = add i32 %i2, 1
  br label %hdr2

hdr3:
  %i3 = phi i32 [0, %body2], [%inc3, %latch3]
  %c3 = icmp slt i32 %i3, %n
  br i1 %c3, label %body3, label %exit3
body3:
  %inner = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
                  ptr inttoptr (i64 4444 to ptr), i32 16)
               to label %ib unwind label %u
ib:
  call void @sink(ptr addrspace(1) %inner)
  br label %latch3
latch3:
  %inc3 = add i32 %i3, 1
  br label %hdr3

exit3:
  br label %latch2
exit2:
  br label %latch1
exit1:
  ret void

u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; Default cutoff (20): max-depth 3 < 20, Regular mode, normal handling.
; DEFAULT-LABEL: define void @test_cutoff
; DEFAULT: invoke {{.*}}@jeandle.new_instance({{.*}}i64 4444
; DEFAULT: call void @sink

; Override cutoff to 2: max-depth 3 > 2, StopNew, alloc refused.
; OVERRIDE-LABEL: define void @test_cutoff
; OVERRIDE: invoke {{.*}}@jeandle.new_instance({{.*}}i64 4444
; OVERRIDE: call void @sink

!java-method-compilation = !{}
