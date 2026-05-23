; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; PEA: alloc + store + load (folded to the stored constant) + use of
; the load + escape. The load fold survives the escape: %v becomes the
; constant 99 (so use_int gets a constant), and the object is materialized
; as an InvokeInst (block-split) hoisted back to the original allocation
; point — the analyzer chooses the alloc's normal-dest start as the safe
; insertion point so the new invoke dominates every existing use of the
; original allocation (soundness against in-block forward references). The
; field store is replayed on the normal-dest of the materialization invoke,
; ahead of the load-fold consumer and the eventual sink call.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @use_int(i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_load_then_escape() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %n unwind label %u
n:
  %s = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  store atomic i32 99, ptr addrspace(1) %s unordered, align 4
  %v = load atomic i32, ptr addrspace(1) %s unordered, align 4
  call void @use_int(i32 %v)
  call void @sink(ptr addrspace(1) %o)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @test_load_then_escape
; CHECK: %[[MAT:[A-Za-z0-9._]+]] = invoke hotspotcc{{.*}}ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 12345 to ptr), i32 16)
; CHECK-NEXT: to label %{{.*}} unwind label %{{.*}}
; CHECK: store atomic i32 99, ptr addrspace(1) %{{.*}} unordered, align {{[0-9]+}}
; CHECK: call void @use_int(i32 99)
; CHECK: call void @sink(ptr addrspace(1) %[[MAT]])

!java-method-compilation = !{}
