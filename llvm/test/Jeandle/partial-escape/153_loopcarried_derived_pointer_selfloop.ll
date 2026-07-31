; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s
;
; TRUE self-loop (the header is its own back-edge predecessor) carrying a
; DERIVED pointer %sf = gep %X, 8. A call-form allocation is used so the alloc
; lives inside the self-loop header (mirrors 145_loopcarried_inbody_selfloop).
; Exercises the hardest timing case for the re-derivation: the back-edge pred is
; the header itself, so the per-pred materialization + re-derive placement runs
; against the header terminator.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_153_selfloop(i32 %n) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  br label %hdr
hdr:
  %i = phi i32 [ 0, %entry ], [ %i1, %hdr ]
  %psf = phi ptr addrspace(1) [ null, %entry ], [ %sf, %hdr ]
  %X = call hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 5555 to ptr), i32 16)
  %sf = getelementptr inbounds i8, ptr addrspace(1) %X, i64 8
  store atomic i32 %i, ptr addrspace(1) %sf unordered, align 4
  %i1 = add i32 %i, 1
  %c = icmp slt i32 %i, %n
  br i1 %c, label %hdr, label %exit
exit:
  %ec = icmp eq ptr addrspace(1) %psf, null
  br i1 %ec, label %done, label %obs
obs:
  call void @sink(ptr addrspace(1) %psf)
  br label %done
done:
  ret void
}

; The carried derived pointer is materialized at the back-edge and re-derived;
; the exit @sink receives a valid (non-poison) carried pointer.
; CHECK-LABEL: define void @test_153_selfloop
; CHECK: call void @sink(ptr addrspace(1) %psf)
; CHECK-NOT: poison

!java-method-compilation = !{}
