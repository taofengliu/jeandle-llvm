; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s
;
; A loop-LOCAL object %X allocated in the body is carried across the back-edge
; by the header pointer-PHI %px and escapes at the loop exit (@sink). The field
; stored is a live SSA value (%i), so materialization is straightforward.
;
; This is the Graal-aligned outcome: %X is virtualized within the iteration and
; MATERIALISED AT THE BACK-EDGE (the latch's end node) -- Graal's
; ensureMaterialized at predecessor.getEndNode() (PartialEscapeClosure.java
; :996/1504), effects kept on convergence (EffectsClosure.java:472-474). The
; carried PHI %px is rewired to the materialized value; the original body alloc
; is eliminated. No poison.
;
; The header has a single forward predecessor (entry), so this exercises the
; fixpoint path's post-body merge directly (no loop-simplify needed).

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_143_inbody_carried(i32 %n) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  br label %hdr
hdr:
  %i = phi i32 [ 0, %entry ], [ %i1, %latch ]
  %px = phi ptr addrspace(1) [ null, %entry ], [ %X, %latch ]
  %c = icmp slt i32 %i, %n
  br i1 %c, label %body, label %exit
body:
  %X = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 5555 to ptr), i32 16)
          to label %bcont unwind label %u
bcont:
  %sf = getelementptr inbounds i8, ptr addrspace(1) %X, i64 8
  store atomic i32 %i, ptr addrspace(1) %sf unordered, align 4
  br label %latch
latch:
  %i1 = add i32 %i, 1
  br label %hdr
exit:
  %ec = icmp eq ptr addrspace(1) %px, null
  br i1 %ec, label %done, label %obs
obs:
  call void @sink(ptr addrspace(1) %px)
  br label %done
done:
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; The carried object is materialised at the back-edge (a pea.mat invoke), and the
; carried PHI's back-edge incoming is rewired to that materialised value (not
; poison, not the eliminated original alloc). No poison anywhere.
; CHECK-LABEL: define void @test_143_inbody_carried
; CHECK: %px = phi ptr addrspace(1) [ null, %entry ], [ %pea.mat, %mat.cont ]
; CHECK: %pea.mat = invoke
; CHECK: call void @sink(ptr addrspace(1) %px)
; CHECK-NOT: poison

!java-method-compilation = !{}
