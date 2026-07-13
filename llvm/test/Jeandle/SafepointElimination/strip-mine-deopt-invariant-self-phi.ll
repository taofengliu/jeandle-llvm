; RUN: opt -passes='safepoint-elimination<early>,safepoint-elimination<strip-mining>,verify<jeandle-safepoint-coverage>' \
; RUN:   -jeandle-enable-strip-mining -jeandle-verify-safepoint-coverage=fatal \
; RUN:   -S < %s | FileCheck %s
; RUN: opt -passes='function(safepoint-elimination<early>,safepoint-elimination<strip-mining>),java-operation-lower<phase=1>,rewrite-statepoints-for-gc,verify' \
; RUN:   -jeandle-enable-strip-mining -S < %s | FileCheck %s --check-prefix=STATEPOINT

; A frontend can represent an unchanged local such as `this` with a header phi
; whose latch value is the phi itself. Although syntactically loop-carried, the
; value is invariant and is safe to lift into the relocated poll's outer state.

declare hotspotcc void @safepoint_handler()

define private hotspotcc void @jeandle.safepoint_poll() #0 {
entry:
  call hotspotcc void @safepoint_handler() [ "deopt"() ]
  ret void
}

define void @invariant_self_phi(i64 %n, ptr addrspace(1) %receiver) gc "hotspotgc" {
entry:
  br label %header

header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %latch ]
  %this = phi ptr addrspace(1) [ %receiver, %entry ], [ %this, %latch ]
  %cond = icmp slt i64 %iv, %n
  br i1 %cond, label %body, label %exit

body:
  %iv.next = add i64 %iv, 1
  call hotspotcc void @jeandle.safepoint_poll()
      [ "deopt"(ptr addrspace(1) %this, i64 %iv.next) ]
  br label %latch

latch:
  br label %header

exit:
  ret void
}

!java-method-compilation = !{}

attributes #0 = { "lower-phase"="1" "noinline" }

; CHECK-LABEL: @invariant_self_phi(
; CHECK:       body:
; CHECK-NOT:     call hotspotcc void @jeandle.safepoint_poll
; CHECK:       header.outer:
; CHECK:         %this.outer = phi ptr addrspace(1) [ %receiver, %header.outer.ph ], [ %this.outer.next, %header.outer.latch ]
; CHECK:       header.outer.latch:
; CHECK:         %this.outer.next = phi ptr addrspace(1) [ %this, %header ]
; CHECK:         call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(ptr addrspace(1) %this.outer.next, i64 %outer.iv.next) ]{{.*}}!poll-coverage

; STATEPOINT-LABEL: @invariant_self_phi(
; STATEPOINT:       header:
; STATEPOINT:         %[[IV:[^ ]+]] = phi i64
; STATEPOINT:         %[[THIS:[^ ]+]] = phi ptr addrspace(1)
; STATEPOINT:       header.outer:
; STATEPOINT:         %[[THIS_OUTER:[^ ]+]] = phi ptr addrspace(1) [ %receiver, %header.outer.ph ], [ %[[RELOCATED:[^, ]+]], %header.outer.latch ]
; STATEPOINT:       header.outer.latch:
; STATEPOINT:         %[[TOKEN:[^ ]+]] = call hotspotcc token {{.*}}@llvm.experimental.gc.statepoint.p0(
; STATEPOINT-SAME:      ptr elementtype(void ()) @safepoint_handler
; STATEPOINT-SAME:      [ "deopt"(ptr addrspace(1) %[[THIS]], i64 %[[IV]]), "gc-live"(ptr addrspace(1) %[[THIS]]) ]
; STATEPOINT-NEXT:    %[[RELOCATED]] = call coldcc ptr addrspace(1) @llvm.experimental.gc.relocate.p1(token %[[TOKEN]], i32 0, i32 0)
