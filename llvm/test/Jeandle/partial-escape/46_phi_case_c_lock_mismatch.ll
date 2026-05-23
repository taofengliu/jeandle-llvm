; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; PEA Case C lock-mismatch — diamond CFG, both arms allocate the same Klass,
; but the left arm enters a monitor on its alloc (live lock count = 1 at
; pred exit) while the right arm does not (lock count = 0). The lock-state
; compatibility check in synthesizeCaseC fails, so Case C is rejected and
; the analyzer falls through to Case A — both virtuals materialize at their
; respective predecessor terminators. (Strict lock order cascade applies on
; the left, but since there's only one locked virtual, no extra cascade.)
;
; Expected: both per-pred allocations survive in IR (materialized at preds);
; the LLVM PHI also survives, merging the two materialized invokes.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc i1 @jeandle.monitorenter_with_thin_lock(ptr addrspace(1), ptr)
declare hotspotcc i1 @jeandle.monitorexit_with_thin_lock(ptr addrspace(1), ptr)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_casec_lock_mismatch(i1 %c)
    gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %lock = alloca i64, align 8
  br i1 %c, label %left, label %right
left:
  %o1 = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
        to label %lcont unwind label %u
lcont:
  ; Enter monitor on the left arm so left.LockCount == 1 at exit.
  %enter = call hotspotcc i1 @jeandle.monitorenter_with_thin_lock(
                ptr addrspace(1) %o1, ptr %lock)
  br label %merge
right:
  %o2 = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
        to label %rcont unwind label %u
rcont:
  br label %merge
merge:
  %p = phi ptr addrspace(1) [ %o1, %lcont ], [ %o2, %rcont ]
  call void @sink(ptr addrspace(1) %p)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; Lock mismatch -> Case A fall-through -> both arms materialize at preds.
; CHECK-LABEL: define void @test_casec_lock_mismatch
; CHECK: invoke hotspotcc {{.*}}@jeandle.new_instance
; CHECK: invoke hotspotcc {{.*}}@jeandle.new_instance
; CHECK: phi ptr addrspace(1)
; CHECK: call void @sink

!java-method-compilation = !{}
