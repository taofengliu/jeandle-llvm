; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Dangling VORef after post-description ineligibility (review §3 #8). At S1,
; %outer is described in the deopt bundle with its field as a VORef to
; %inner (transitive closure describes both). AFTER S1, %inner escapes via a
; DERIVED pointer (%gep) and is marked ineligible. Pre-fix, commit dropped
; %inner's descriptor but kept %outer's — the outer's VORef to %inner's
; vo-id dangled (JDK-side deferred-voref assert / nullptr field). The commit
; DeoptRefDeps cascade now drags %outer to ineligible as well: both
; allocations stay real, all stores survive, and the bundle keeps the live
; %outer oop with NO descriptor at all.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(i32)
declare void @sinkp(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @dangling_voref2(i32 %x) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %inner = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 200 to ptr), i32 16)
       to label %n1 unwind label %u
n1:
  %if1 = getelementptr inbounds i8, ptr addrspace(1) %inner, i64 8
  store atomic i32 %x, ptr addrspace(1) %if1 unordered, align 4
  %outer = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 100 to ptr), i32 24)
       to label %n2 unwind label %u
n2:
  %of2 = getelementptr inbounds i8, ptr addrspace(1) %outer, i64 16
  store atomic ptr addrspace(1) %inner, ptr addrspace(1) %of2 unordered, align 8
  ; S1: outer root; inner transitive VORef member -> both described.
  call void @sink(i32 %x)
       [ "deopt"(i32 99, i32 99, i64 12, ptr addrspace(1) %outer) ]
  ; AFTER S1: derived-pointer escape of inner -> markVirtualOperandsIneligible
  %gep = getelementptr inbounds i8, ptr addrspace(1) %inner, i64 8
  call void @sinkp(ptr addrspace(1) %gep)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; Both objects kept real; stores survive; the sink bundle keeps the live
; %outer oop verbatim; no descriptor (no ScalarValueType / VORef encodings);
; no poison.
; CHECK-LABEL: define void @dangling_voref2(
; CHECK: invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 200 to ptr), i32 16)
; CHECK: store atomic i32 %x, ptr addrspace(1) %if1 unordered, align 4
; CHECK: invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 100 to ptr), i32 24)
; CHECK: store atomic ptr addrspace(1) %inner, ptr addrspace(1) %of2 unordered, align 8
; CHECK: call void @sink(i32 %x) [ "deopt"(i32 99, i32 99, i64 12, ptr addrspace(1) %outer) ]
; CHECK-NOT: i64 262156
; CHECK-NOT: i64 524300
; CHECK-NOT: poison

!java-method-compilation = !{}
