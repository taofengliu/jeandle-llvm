; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s
;
; A DERIVED pointer carried across the back-edge where the derivation is a
; bitcast (offset 0): %sf = bitcast %X. The re-derivation replays offset 0, so
; the carried PHI's back-edge incoming reuses the materialized object directly
; (no extra GEP). The body bitcast is dead-code swept away.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_150_carried_bitcast(i32 %n) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  br label %hdr
hdr:
  %i = phi i32 [ 0, %entry ], [ %i1, %latch ]
  %psf = phi ptr addrspace(1) [ null, %entry ], [ %sf, %latch ]
  %c = icmp slt i32 %i, %n
  br i1 %c, label %body, label %exit
body:
  %X = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 5555 to ptr), i32 16)
          to label %bcont unwind label %u
bcont:
  %sf = bitcast ptr addrspace(1) %X to ptr addrspace(1)
  store atomic i32 %i, ptr addrspace(1) %sf unordered, align 4
  br label %latch
latch:
  %i1 = add i32 %i, 1
  br label %hdr
exit:
  %ec = icmp eq ptr addrspace(1) %psf, null
  br i1 %ec, label %done, label %obs
obs:
  call void @sink(ptr addrspace(1) %psf)
  br label %done
done:
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @test_150_carried_bitcast
; CHECK: %psf = phi ptr addrspace(1) [ null, %entry ], [ %pea.mat, %mat.cont ]
; CHECK: %pea.mat = invoke
; CHECK: call void @sink(ptr addrspace(1) %psf)
; CHECK-NOT: poison

!java-method-compilation = !{}
