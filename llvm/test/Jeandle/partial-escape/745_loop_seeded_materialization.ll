; RUN: opt -S -verify-each -passes=partial-escape-iterative \
; RUN:   -jeandle-vm-callback-log=%S/Inputs/745_loop_seeded_materialization.cblog \
; RUN:   %s | FileCheck %s
; RUN: opt -disable-output -verify-each -passes=partial-escape-iterative \
; RUN:   -jeandle-vm-callback-log=%S/Inputs/745_loop_seeded_materialization.cblog \
; RUN:   -jeandle-trace-pea %s 2>&1 | FileCheck %s --check-prefix=TRACE

; A materialization discovered while converging an inner loop seeds the outer
; retry.  The outer retry must start from its prior merged header state while
; processing inner loops from fresh edge states for that round, without
; retaining replay effects emitted by the earlier traversal.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc void @jeandle.safepoint_poll()

define ptr addrspace(1) @loop_seeded_materialization(i1 %first.done,
                                                      i1 %second.done)
    gc "hotspotgc" personality ptr null {
entry:
  %object = call hotspotcc ptr addrspace(1) @jeandle.new_instance(
      ptr inttoptr (i64 74501 to ptr), i32 0)
  store i8 0, ptr addrspace(1) %object, align 1
  br label %outer.preheader

outer.preheader:
  br label %outer.header

outer.header:
  br label %inner.first.header

inner.first.header:
  br i1 %first.done, label %between.inners, label %inner.first.safepoint

inner.first.safepoint:
  invoke hotspotcc void @jeandle.safepoint_poll()
      to label %inner.first.header unwind label %unwind.first

unwind.first:
  %first.exception = landingpad i64 cleanup
  ret ptr addrspace(1) null

between.inners:
  br label %inner.second.header

inner.second.header:
  br i1 %second.done, label %late, label %inner.second.safepoint

inner.second.safepoint:
  invoke hotspotcc void @jeandle.safepoint_poll()
      to label %inner.second.header unwind label %unwind.second

unwind.second:
  %second.exception = landingpad i64 cleanup
  ret ptr addrspace(1) null

late:
  store i32 0, ptr addrspace(1) %object, align 4
  br label %outer.header
}

; The seeded field is replayed once on the outer forward edge.
; CHECK-LABEL: define ptr addrspace(1) @loop_seeded_materialization(
; CHECK: entry:
; CHECK: store atomic i8 0, ptr addrspace(1) [[OBJECT:%.*]] unordered, align 1
; CHECK: br label %outer.header
; CHECK-NOT: inner.first.header.pea.replay:
; CHECK: inner.first.header:
; CHECK-NOT: inner.second.header.pea.replay:
; CHECK: inner.second.header:
; CHECK-NOT: pea.replay:

; TRACE: PEA: EliminateAllocation function=@loop_seeded_materialization [VO=0]
; TRACE: PEA: Materialize function=@loop_seeded_materialization [VO=0] block=%entry
; TRACE-NOT: block=%inner.first.header.pea.replay
; TRACE-NOT: block=%inner.second.header.pea.replay

!java-method-compilation = !{}
