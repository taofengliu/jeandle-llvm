; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; R10.X1b: lock-stack identity mismatch where the SAME enter call site is
; reached by two preds but each pred carries a DIFFERENT bytecode depth
; via metadata. This is only possible because the test attaches distinct
; `!jeandle.lock_depth` metadata at two distinct IR call sites that
; ultimately hold the same virtual; the per-element compare
;   S[i].Call == RefStack[i].Call && S[i].BytecodeDepth == RefStack[i].BytecodeDepth
; sees Call agreement but BytecodeDepth mismatch and routes to per-pred
; materialise.
;
; Before R10.X1b: BytecodeDepth was not consulted, the depth mismatch was
; ignored, and the two preds would have merged silently with whichever
; pred-side stack appeared first — semantically wrong because a downstream
; un-elide would emit the wrong number of monitorenter calls for some
; paths.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc i1 @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1), ptr)
declare hotspotcc i1 @jeandle.monitorexit_with_lightweight_lock(ptr addrspace(1), ptr)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_lockdepth_per_pred_diff_depth(i1 %cond) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %lock_t = alloca i64, align 8
  %lock_e = alloca i64, align 8
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 8765 to ptr), i32 16)
       to label %dispatch unwind label %u
dispatch:
  br i1 %cond, label %t, label %e
t:
  ; Then-arm: this enter is the only one on %o, so its bytecode depth is 0.
  %et = call hotspotcc i1 @jeandle.monitorenter_with_lightweight_lock(
                  ptr addrspace(1) %o, ptr %lock_t), !jeandle.lock_depth !{i32 0}
  br label %merge
e:
  ; Else-arm: a hypothetical outer synchronized region in the Java source
  ; meant THIS enter would be at depth=1. Different call site, different
  ; depth.
  %ee = call hotspotcc i1 @jeandle.monitorenter_with_lightweight_lock(
                  ptr addrspace(1) %o, ptr %lock_e), !jeandle.lock_depth !{i32 1}
  br label %merge
merge:
  call void @sink(ptr addrspace(1) %o)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; After R10.X1b, %o is materialised at each pred (depth-mismatch routes
; the merge through per-pred materialise instead of the legacy bail).
; The original allocation is replaced by the per-pred materialised invokes.
; CHECK-LABEL: define void @test_lockdepth_per_pred_diff_depth
; Per-pred materialise: TWO new_instance invokes survive, one per arm.
; CHECK-DAG: %[[MATT:[A-Za-z0-9._]+]] = invoke hotspotcc{{.*}}ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 8765 to ptr), i32 16)
; CHECK-DAG: %[[MATE:[A-Za-z0-9._]+]] = invoke hotspotcc{{.*}}ptr addrspace(1) @jeandle.new_instance(ptr inttoptr (i64 8765 to ptr), i32 16)
; Both pred-side enter calls survive on per-pred materialised pointers.
; CHECK-DAG: call hotspotcc i1 @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1) {{.*}}, ptr %lock_t)
; CHECK-DAG: call hotspotcc i1 @jeandle.monitorenter_with_lightweight_lock(ptr addrspace(1) {{.*}}, ptr %lock_e)
; The merge block synthesises a phi over the two pred materialised pointers
; and the sink uses it.
; CHECK: phi ptr addrspace(1)
; CHECK: call void @sink(ptr addrspace(1)

!java-method-compilation = !{}
