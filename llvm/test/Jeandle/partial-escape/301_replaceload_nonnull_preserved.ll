; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; When PEA virtualizes a reference field by RAUW-ing the field-load onto
; the previously-stored value, transfer load-only metadata (`!nonnull`
; etc.) from the original load onto the replacement load if the
; replacement is itself a LoadInst that lacks it. This preserves the
; precise "not-null" knowledge for subsequent LLVM passes after PEA
; snaps away the field-load.
;
; Setup:
;   %src    = load ptr addrspace(1), ptr addrspace(1) %external  ; NO !nonnull
;   store %src into %obj.field
;   %back   = load ptr addrspace(1), ptr addrspace(1) %slot, !nonnull
;   ... use %back ...
;
; PEA virtualizes %obj (the only escape of %back is via @consume, which is
; rewritten to consume the materialized pointer; but the OBJECT itself is
; fully virtualized in this single-block setting, so the field-load gets
; eliminated and its uses are RAUW'd onto %src). After metadata
; preservation, %src acquires the !nonnull metadata that %back had carried.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @consume(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_301(ptr addrspace(1) %external)
    gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %obj = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
         to label %n unwind label %u
n:
  ; Source load — NOT marked nonnull. Becomes the Replacement for %back.
  %src = load atomic ptr addrspace(1), ptr addrspace(1) %external unordered, align 8
  %slot = getelementptr inbounds i8, ptr addrspace(1) %obj, i64 8
  store atomic ptr addrspace(1) %src, ptr addrspace(1) %slot unordered, align 8
  ; Target load — marked nonnull. After PEA RAUWs it onto %src, the
  ; metadata-copy step transfers !nonnull onto %src.
  %back = load atomic ptr addrspace(1), ptr addrspace(1) %slot unordered, align 8, !nonnull !{}
  call void @consume(ptr addrspace(1) %back)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; The original allocation, store and second load are all eliminated; only
; %src and its !nonnull-carrying replacement survive. The metadata copy
; ensures the surviving %src load carries the !nonnull metadata.
; CHECK-LABEL: define void @test_301
; CHECK-NOT: jeandle.new_instance
; CHECK: %src = load atomic ptr addrspace(1), ptr addrspace(1) %external unordered, align 8{{.*}}!nonnull
; CHECK-NEXT: call void @consume(ptr addrspace(1) %src)

!java-method-compilation = !{}
