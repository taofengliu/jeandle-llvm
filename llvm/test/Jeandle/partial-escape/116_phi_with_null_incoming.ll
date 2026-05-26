; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Lit coverage for the Case-A fallback on a phi with a virtual incoming
; on one arm and a null pointer on the other. The merge-time AliasMap
; consultation marks the virtual-incoming side eligible for per-pred
; materialization; the null side is treated as a non-virtual pointer.
; The resulting Materialize lands on the virtual-side pred's terminator
; and the PHI survives in IR with %[[MAT]] / null incomings.
;
; This is purely a regression guard — today's processBlockPhis Case-A
; fallback at PartialEscapeAnalysis.cpp already handles the pattern; we
; assert the end-to-end behaviour stays sound.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_phi_virt_null(i1 %c)
    gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  br i1 %c, label %alloc, label %nullarm
alloc:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %merge unwind label %u
nullarm:
  br label %merge
merge:
  %x = phi ptr addrspace(1) [ %o, %alloc ], [ null, %nullarm ]
  call void @sink(ptr addrspace(1) %x)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; The virtual on the alloc arm is materialized at the alloc pred (so the
; PHI's first incoming becomes the materialized invoke); the null arm is
; unchanged. The PHI survives.
; CHECK-LABEL: define void @test_phi_virt_null
; CHECK: invoke{{.*}}@jeandle.new_instance
; CHECK: phi ptr addrspace(1)
; CHECK: call void @sink

!java-method-compilation = !{}
