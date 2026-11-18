; RUN: opt -passes='safepoint-poll-elimination<after-strip-mining>,verify<jeandle-safepoint-coverage>' -jeandle-safepoint-max-work=0 -jeandle-verify-safepoint-coverage=fatal -S %s | FileCheck %s --check-prefix=LIMIT
; RUN: opt -passes='loop-simplify,lcssa,safepoint-strip-mining<strip-mining>,safepoint-poll-elimination<after-strip-mining>,verify<jeandle-safepoint-coverage>' -jeandle-verify-safepoint-coverage=fatal -verify-each -S %s | FileCheck %s
; RUN: opt -passes='early-cse,instcombine,simplifycfg,loop-mssa(licm<no-allowspeculation>,loop-rotate,licm,indvars),safepoint-strip-mining<strip-mining>,safepoint-poll-elimination<after-strip-mining>,verify<jeandle-safepoint-coverage>' -jeandle-verify-safepoint-coverage=fatal -verify-each -S %s | FileCheck %s

; CHECK-LABEL: define void @branch_calls(
; CHECK-NOT: .outer
; CHECK-NOT: call hotspotcc void @jeandle.safepoint_poll
; CHECK: call hotspotcc void @left_java_method
; CHECK-NOT: call hotspotcc void @jeandle.safepoint_poll
; CHECK: call hotspotcc void @right_java_method
; CHECK-NOT: call hotspotcc void @jeandle.safepoint_poll
; CHECK: ret void

; LIMIT-LABEL: @branch_calls(
; LIMIT-NOT: ret void
; LIMIT: call hotspotcc void @jeandle.safepoint_poll()

; Every backedge crosses one guaranteed Java call, but no call dominates it.
declare hotspotcc void @jeandle.safepoint_poll()
declare hotspotcc void @left_java_method()
declare hotspotcc void @right_java_method()

define void @branch_calls(ptr %flag, i64 %n) "java-method" {
entry:
  br label %header
header:
  %iv = phi i64 [0, %entry], [%next, %latch]
  %choose = load volatile i1, ptr %flag
  br i1 %choose, label %left, label %right
left:
  call hotspotcc void @left_java_method() ["deopt"(i64 %iv)]
  br label %latch
right:
  call hotspotcc void @right_java_method() ["deopt"(i64 %iv)]
  br label %latch
latch:
  %next = add i64 %iv, 1
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i64 %next)]
  %continue = icmp slt i64 %next, %n
  br i1 %continue, label %header, label %exit
exit:
  ret void
}


declare hotspotcc void @leaf_method() "gc-leaf-function"
; CHECK-LABEL: define void @one_leaf_arm(
; CHECK: call hotspotcc void @leaf_method
; CHECK: call hotspotcc void @jeandle.safepoint_poll

define void @one_leaf_arm(ptr %flag, i64 %n) "java-method" {
entry:
  br label %header
header:
  %iv = phi i64 [0, %entry], [%next, %latch]
  %choose = load volatile i1, ptr %flag
  br i1 %choose, label %left, label %right
left:
  call hotspotcc void @left_java_method() ["deopt"(i64 %iv)]
  br label %latch
right:
  call hotspotcc void @leaf_method() ["deopt"(i64 %iv)]
  br label %latch
latch:
  %next = add i64 %iv, 1
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i64 %next)]
  %continue = icmp slt i64 %next, %n
  br i1 %continue, label %header, label %exit
exit:
  ret void
}


; Calls on separate backedges cover both, even without a unique latch.
; CHECK-LABEL: define void @separate_backedges(
; CHECK-NOT: call hotspotcc void @jeandle.safepoint_poll
; CHECK: call hotspotcc void @left_java_method
; CHECK-NOT: call hotspotcc void @jeandle.safepoint_poll
; CHECK: call hotspotcc void @right_java_method
; CHECK-NOT: call hotspotcc void @jeandle.safepoint_poll
; CHECK: ret void

define void @separate_backedges(ptr %flag, ptr %running) "java-method" {
entry:
  br label %header
header:
  %choose = load volatile i1, ptr %flag
  br i1 %choose, label %left, label %right
left:
  call hotspotcc void @left_java_method() ["deopt"()]
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"()]
  %a = load volatile i1, ptr %running
  br i1 %a, label %header, label %exit
right:
  call hotspotcc void @right_java_method() ["deopt"()]
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"()]
  %b = load volatile i1, ptr %running
  br i1 %b, label %header, label %exit
exit:
  ret void
}
!java-method-compilation = !{}
