; RUN: opt -passes='verify<jeandle-safepoint-coverage>' \
; RUN:   -jeandle-verify-safepoint-coverage=warn -S < %s 2>&1 \
; RUN:   | FileCheck %s
; RUN: not --crash opt -passes='verify<jeandle-safepoint-coverage>' \
; RUN:   -jeandle-verify-safepoint-coverage=fatal -disable-output < %s 2>&1 \
; RUN:   | FileCheck %s --check-prefix=ABORT

; A transform-authored !strip-mined marker is only a candidate certificate.  The
; verifier must still reject marked loops unless the outer poll and bounded inner
; batch are present structurally.

declare hotspotcc void @jeandle.safepoint_poll()
declare void @llvm.assume(i1)
declare i32 @llvm.sadd.sat.i32(i32, i32)
declare i64 @llvm.sadd.sat.i64(i64, i64)
declare i64 @llvm.uadd.sat.i64(i64, i64)

define void @forged_marker_no_outer(i64 %n) {
entry:
  br label %forged.header

forged.header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %forged.latch ]
  %cond = icmp slt i64 %iv, %n
  br i1 %cond, label %forged.body, label %forged.exit

forged.body:
  br label %forged.latch

forged.latch:
  %iv.next = add i64 %iv, 1
  br label %forged.header, !strip-mined !0

forged.exit:
  ret void
}

define void @forged_marker_no_outer_poll(i64 %n) {
entry:
  br label %nopoll.outer

nopoll.outer:
  %outer.iv = phi i64 [ 0, %entry ], [ %outer.iv.next, %nopoll.outer.latch ]
  %outer.cond = icmp slt i64 %outer.iv, %n
  br i1 %outer.cond, label %nopoll.inner.entry, label %nopoll.exit

nopoll.inner.entry:
  %batch.end = call i64 @llvm.sadd.sat.i64(i64 %outer.iv, i64 1000)
  %cap.cond = icmp slt i64 %batch.end, %n
  %inner.limit = select i1 %cap.cond, i64 %batch.end, i64 %n
  br label %nopoll.inner.header

nopoll.inner.header:
  %iv = phi i64 [ %outer.iv, %nopoll.inner.entry ], [ %iv.next, %nopoll.inner.latch ]
  %cond = icmp slt i64 %iv, %inner.limit
  br i1 %cond, label %nopoll.inner.body, label %nopoll.outer.latch

nopoll.inner.body:
  br label %nopoll.inner.latch

nopoll.inner.latch:
  %iv.next = add i64 %iv, 1
  br label %nopoll.inner.header, !strip-mined !0

nopoll.outer.latch:
  %outer.iv.next = phi i64 [ %iv, %nopoll.inner.header ]
  br label %nopoll.outer

nopoll.exit:
  ret void
}

define void @forged_marker_bad_batch(i64 %n) {
entry:
  br label %bad.outer

bad.outer:
  %outer.iv = phi i64 [ 0, %entry ], [ %outer.iv.next, %bad.outer.latch ]
  %outer.cond = icmp slt i64 %outer.iv, %n
  br i1 %outer.cond, label %bad.inner.entry, label %bad.exit

bad.inner.entry:
  %batch.end = call i64 @llvm.sadd.sat.i64(i64 %outer.iv, i64 2000)
  %cap.cond = icmp slt i64 %batch.end, %n
  %inner.limit = select i1 %cap.cond, i64 %batch.end, i64 %n
  br label %bad.inner.header

bad.inner.header:
  %iv = phi i64 [ %outer.iv, %bad.inner.entry ], [ %iv.next, %bad.inner.latch ]
  %cond = icmp slt i64 %iv, %inner.limit
  br i1 %cond, label %bad.inner.body, label %bad.outer.latch

bad.inner.body:
  br label %bad.inner.latch

bad.inner.latch:
  %iv.next = add i64 %iv, 1
  br label %bad.inner.header, !strip-mined !0

bad.outer.latch:
  %outer.iv.next = phi i64 [ %iv, %bad.inner.header ]
  call hotspotcc void @jeandle.safepoint_poll(), !poll-coverage !0
  br label %bad.outer

bad.exit:
  ret void
}

define void @forged_marker_bad_batch_start(i64 %n) {
entry:
  br label %badstart.outer

badstart.outer:
  %outer.iv = phi i64 [ 0, %entry ], [ %outer.iv.next, %badstart.outer.latch ]
  %outer.cond = icmp slt i64 %outer.iv, %n
  br i1 %outer.cond, label %badstart.inner.entry, label %badstart.exit

badstart.inner.entry:
  %batch.end = call i64 @llvm.sadd.sat.i64(i64 %outer.iv, i64 1000)
  %cap.cond = icmp slt i64 %batch.end, %n
  %inner.limit = select i1 %cap.cond, i64 %batch.end, i64 %n
  br label %badstart.inner.header

badstart.inner.header:
  %iv = phi i64 [ 0, %badstart.inner.entry ], [ %iv.next, %badstart.inner.latch ]
  %cond = icmp slt i64 %iv, %inner.limit
  br i1 %cond, label %badstart.inner.body, label %badstart.outer.latch

badstart.inner.body:
  br label %badstart.inner.latch

badstart.inner.latch:
  %iv.next = add i64 %iv, 1
  br label %badstart.inner.header, !strip-mined !0

badstart.outer.latch:
  %outer.iv.next = phi i64 [ %iv, %badstart.inner.header ]
  call hotspotcc void @jeandle.safepoint_poll(), !poll-coverage !0
  br label %badstart.outer

badstart.exit:
  ret void
}

define void @forged_latch_marker_no_outer_poll(i64 %n) {
entry:
  br label %latchnopoll.outer

latchnopoll.outer:
  %outer.iv = phi i64 [ 0, %entry ], [ %outer.iv.next, %latchnopoll.outer.latch ]
  %outer.cond = icmp slt i64 %outer.iv, %n
  br i1 %outer.cond, label %latchnopoll.inner.entry, label %latchnopoll.exit

latchnopoll.inner.entry:
  %batch.end = call i64 @llvm.sadd.sat.i64(i64 %outer.iv, i64 1000)
  %cap.cond = icmp slt i64 %batch.end, %n
  %inner.limit = select i1 %cap.cond, i64 %batch.end, i64 %n
  br label %latchnopoll.inner.header

latchnopoll.inner.header:
  %iv = phi i64 [ %outer.iv, %latchnopoll.inner.entry ], [ %iv.next, %latchnopoll.inner.latch ]
  br label %latchnopoll.inner.latch

latchnopoll.inner.latch:
  %iv.next = add i64 %iv, 1
  %cond = icmp slt i64 %iv.next, %inner.limit
  br i1 %cond, label %latchnopoll.inner.header, label %latchnopoll.outer.latch, !strip-mined !0

latchnopoll.outer.latch:
  %outer.iv.next = phi i64 [ %iv.next, %latchnopoll.inner.latch ]
  br label %latchnopoll.outer

latchnopoll.exit:
  ret void
}

define void @forged_latch_marker_bad_resume(i64 %n) {
entry:
  br label %latchbad.outer

latchbad.outer:
  %outer.iv = phi i64 [ 0, %entry ], [ %outer.iv.next, %latchbad.outer.latch ]
  %outer.cond = icmp slt i64 %outer.iv, %n
  br i1 %outer.cond, label %latchbad.inner.entry, label %latchbad.exit

latchbad.inner.entry:
  %batch.end = call i64 @llvm.sadd.sat.i64(i64 %outer.iv, i64 1000)
  %cap.cond = icmp slt i64 %batch.end, %n
  %inner.limit = select i1 %cap.cond, i64 %batch.end, i64 %n
  br label %latchbad.inner.header

latchbad.inner.header:
  %iv = phi i64 [ %outer.iv, %latchbad.inner.entry ], [ %iv.next, %latchbad.inner.latch ]
  br label %latchbad.inner.latch

latchbad.inner.latch:
  %iv.next = add i64 %iv, 1
  %cond = icmp slt i64 %iv.next, %inner.limit
  br i1 %cond, label %latchbad.inner.header, label %latchbad.outer.latch, !strip-mined !0

latchbad.outer.latch:
  %outer.iv.next = phi i64 [ %iv, %latchbad.inner.latch ]
  call hotspotcc void @jeandle.safepoint_poll(), !poll-coverage !0
  br label %latchbad.outer

latchbad.exit:
  ret void
}

define void @forged_marker_ne_bad_batch(i64 %n) {
entry:
  br label %nebad.outer

nebad.outer:
  %outer.iv = phi i64 [ 0, %entry ], [ %outer.iv.next, %nebad.outer.latch ]
  %outer.cond = icmp slt i64 %outer.iv, %n
  br i1 %outer.cond, label %nebad.inner.entry, label %nebad.exit

nebad.inner.entry:
  %batch.end = call i64 @llvm.sadd.sat.i64(i64 %outer.iv, i64 2000)
  %cap.cond = icmp slt i64 %batch.end, %n
  %inner.limit = select i1 %cap.cond, i64 %batch.end, i64 %n
  br label %nebad.inner.header

nebad.inner.header:
  %iv = phi i64 [ %outer.iv, %nebad.inner.entry ], [ %iv.next, %nebad.inner.latch ]
  %cond = icmp ne i64 %iv, %inner.limit
  br i1 %cond, label %nebad.inner.body, label %nebad.outer.latch

nebad.inner.body:
  br label %nebad.inner.latch

nebad.inner.latch:
  %iv.next = add i64 %iv, 1
  br label %nebad.inner.header, !strip-mined !0

nebad.outer.latch:
  %outer.iv.next = phi i64 [ %iv, %nebad.inner.header ]
  call hotspotcc void @jeandle.safepoint_poll(), !poll-coverage !0
  br label %nebad.outer

nebad.exit:
  ret void
}

define void @forged_marker_inclusive_bad_margin(i64 %n) {
entry:
  br label %inclbad.outer

inclbad.outer:
  %outer.iv = phi i64 [ 0, %entry ], [ %outer.iv.next, %inclbad.outer.latch ]
  %outer.cond = icmp sle i64 %outer.iv, %n
  br i1 %outer.cond, label %inclbad.inner.entry, label %inclbad.exit

inclbad.inner.entry:
  %batch.end = call i64 @llvm.sadd.sat.i64(i64 %outer.iv, i64 999)
  %cap.cond = icmp sle i64 %batch.end, %n
  %inner.limit = select i1 %cap.cond, i64 %batch.end, i64 %n
  br label %inclbad.inner.header

inclbad.inner.header:
  %iv = phi i64 [ %outer.iv, %inclbad.inner.entry ], [ %iv.next, %inclbad.inner.latch ]
  %cond = icmp sle i64 %iv, %inner.limit
  br i1 %cond, label %inclbad.inner.body, label %inclbad.outer.latch

inclbad.inner.body:
  br label %inclbad.inner.latch

inclbad.inner.latch:
  %iv.next = add i64 %iv, 1
  br label %inclbad.inner.header, !strip-mined !0

inclbad.outer.latch:
  %outer.iv.next = phi i64 [ %iv, %inclbad.inner.header ]
  call hotspotcc void @jeandle.safepoint_poll(), !poll-coverage !0
  br label %inclbad.outer

inclbad.exit:
  ret void
}

define void @forged_marker_inclusive_wrong_guard(i32 %n) {
entry:
  %wrong.guard = icmp sle i32 %n, 2147483647
  br i1 %wrong.guard, label %wrongguard.outer, label %wrongguard.exit

wrongguard.outer:
  %outer.iv = phi i32 [ 0, %entry ], [ %outer.iv.next, %wrongguard.outer.latch ]
  %outer.cond = icmp sle i32 %outer.iv, %n
  br i1 %outer.cond, label %wrongguard.inner.entry, label %wrongguard.exit

wrongguard.inner.entry:
  %batch.end = call i32 @llvm.sadd.sat.i32(i32 %outer.iv, i32 999)
  %cap.cond = icmp sle i32 %batch.end, %n
  %inner.limit = select i1 %cap.cond, i32 %batch.end, i32 %n
  br label %wrongguard.inner.header

wrongguard.inner.header:
  %iv = phi i32 [ %outer.iv, %wrongguard.inner.entry ], [ %iv.next, %wrongguard.inner.latch ]
  %cond = icmp sle i32 %iv, %inner.limit
  br i1 %cond, label %wrongguard.inner.body, label %wrongguard.outer.latch

wrongguard.inner.body:
  br label %wrongguard.inner.latch

wrongguard.inner.latch:
  %iv.next = add i32 %iv, 1
  br label %wrongguard.inner.header, !strip-mined !0

wrongguard.outer.latch:
  %outer.iv.next = phi i32 [ %iv, %wrongguard.inner.header ]
  call hotspotcc void @jeandle.safepoint_poll(), !poll-coverage !0
  br label %wrongguard.outer

wrongguard.exit:
  ret void
}

; Even the right numeric threshold is not a proof when separate undef uses can
; choose different values for the guard and the real loop limit.
define void @forged_marker_inclusive_unstable_limit() {
entry:
  %guard = icmp slt i32 undef, 2147483647
  br i1 %guard, label %unstable.outer, label %unstable.exit

unstable.outer:
  %outer.iv = phi i32 [ 0, %entry ], [ %outer.iv.next, %unstable.outer.latch ]
  %outer.cond = icmp sle i32 %outer.iv, undef
  br i1 %outer.cond, label %unstable.inner.entry, label %unstable.exit

unstable.inner.entry:
  %batch.end = call i32 @llvm.sadd.sat.i32(i32 %outer.iv, i32 999)
  %cap.cond = icmp sle i32 %batch.end, undef
  %inner.limit = select i1 %cap.cond, i32 %batch.end, i32 undef
  br label %unstable.inner.header

unstable.inner.header:
  %iv = phi i32 [ %outer.iv, %unstable.inner.entry ], [ %iv.next, %unstable.inner.latch ]
  %cond = icmp sle i32 %iv, %inner.limit
  br i1 %cond, label %unstable.inner.body, label %unstable.outer.latch

unstable.inner.body:
  br label %unstable.inner.latch

unstable.inner.latch:
  %iv.next = add i32 %iv, 1
  br label %unstable.inner.header, !strip-mined !0

unstable.outer.latch:
  %outer.iv.next = phi i32 [ %iv, %unstable.inner.header ]
  call hotspotcc void @jeandle.safepoint_poll(), !poll-coverage !0
  br label %unstable.outer

unstable.exit:
  ret void
}

; A single SSA instruction derived from undef is still unstable across its
; transitive uses, even when SCEV can prove its numeric range.
define void @forged_marker_inclusive_masked_undef_limit() {
entry:
  %limit = and i32 undef, 2147483646
  br label %maskedlimit.outer

maskedlimit.outer:
  %outer.iv = phi i32 [ 0, %entry ], [ %outer.iv.next, %maskedlimit.outer.latch ]
  %outer.cond = icmp sle i32 %outer.iv, %limit
  br i1 %outer.cond, label %maskedlimit.inner.entry, label %maskedlimit.exit

maskedlimit.inner.entry:
  %batch.end = call i32 @llvm.sadd.sat.i32(i32 %outer.iv, i32 999)
  %cap.cond = icmp sle i32 %batch.end, %limit
  %inner.limit = select i1 %cap.cond, i32 %batch.end, i32 %limit
  br label %maskedlimit.inner.header

maskedlimit.inner.header:
  %iv = phi i32 [ %outer.iv, %maskedlimit.inner.entry ], [ %iv.next, %maskedlimit.inner.latch ]
  %cond = icmp sle i32 %iv, %inner.limit
  br i1 %cond, label %maskedlimit.inner.body, label %maskedlimit.outer.latch

maskedlimit.inner.body:
  br label %maskedlimit.inner.latch

maskedlimit.inner.latch:
  %iv.next = add i32 %iv, 1
  br label %maskedlimit.inner.header, !strip-mined !0

maskedlimit.outer.latch:
  %outer.iv.next = phi i32 [ %iv, %maskedlimit.inner.header ]
  call hotspotcc void @jeandle.safepoint_poll(), !poll-coverage !0
  br label %maskedlimit.outer

maskedlimit.exit:
  ret void
}

define void @forged_marker_inclusive_masked_undef_start() {
entry:
  %start = and i32 undef, 2147483646
  br label %maskedstart.outer

maskedstart.outer:
  %outer.iv = phi i32 [ %start, %entry ], [ %outer.iv.next, %maskedstart.outer.latch ]
  %outer.cond = icmp sle i32 %outer.iv, 2147483646
  br i1 %outer.cond, label %maskedstart.inner.entry, label %maskedstart.exit

maskedstart.inner.entry:
  %batch.end = call i32 @llvm.sadd.sat.i32(i32 %outer.iv, i32 999)
  %cap.cond = icmp sle i32 %batch.end, 2147483646
  %inner.limit = select i1 %cap.cond, i32 %batch.end, i32 2147483646
  br label %maskedstart.inner.header

maskedstart.inner.header:
  %iv = phi i32 [ %outer.iv, %maskedstart.inner.entry ], [ %iv.next, %maskedstart.inner.latch ]
  %cond = icmp sle i32 %iv, %inner.limit
  br i1 %cond, label %maskedstart.inner.body, label %maskedstart.outer.latch

maskedstart.inner.body:
  br label %maskedstart.inner.latch

maskedstart.inner.latch:
  %iv.next = add i32 %iv, 1
  br label %maskedstart.inner.header, !strip-mined !0

maskedstart.outer.latch:
  %outer.iv.next = phi i32 [ %iv, %maskedstart.inner.header ]
  call hotspotcc void @jeandle.safepoint_poll(), !poll-coverage !0
  br label %maskedstart.outer

maskedstart.exit:
  ret void
}

define void @forged_marker_uinclusive_bad_margin() {
entry:
  br label %uinclbad.outer

uinclbad.outer:
  %outer.iv = phi i64 [ 0, %entry ], [ %outer.iv.next, %uinclbad.outer.latch ]
  %outer.cond = icmp ule i64 %outer.iv, -2
  br i1 %outer.cond, label %uinclbad.inner.entry, label %uinclbad.exit

uinclbad.inner.entry:
  %batch.end = call i64 @llvm.uadd.sat.i64(i64 %outer.iv, i64 1998)
  %cap.cond = icmp ule i64 %batch.end, -2
  %inner.limit = select i1 %cap.cond, i64 %batch.end, i64 -2
  br label %uinclbad.inner.header

uinclbad.inner.header:
  %iv = phi i64 [ %outer.iv, %uinclbad.inner.entry ], [ %iv.next, %uinclbad.inner.latch ]
  %cond = icmp ule i64 %iv, %inner.limit
  br i1 %cond, label %uinclbad.inner.body, label %uinclbad.outer.latch

uinclbad.inner.body:
  br label %uinclbad.inner.latch

uinclbad.inner.latch:
  %iv.next = add i64 %iv, 2
  br label %uinclbad.inner.header, !strip-mined !0

uinclbad.outer.latch:
  %outer.iv.next = phi i64 [ %iv, %uinclbad.inner.header ]
  call hotspotcc void @jeandle.safepoint_poll(), !poll-coverage !0
  br label %uinclbad.outer

uinclbad.exit:
  ret void
}

define void @forged_marker_uexclusive_step2_wrap() {
entry:
  br label %uexclbad.outer

uexclbad.outer:
  %outer.iv = phi i64 [ 0, %entry ], [ %outer.iv.next, %uexclbad.outer.latch ]
  %outer.cond = icmp ult i64 %outer.iv, -1
  br i1 %outer.cond, label %uexclbad.inner.entry, label %uexclbad.exit

uexclbad.inner.entry:
  %batch.end = call i64 @llvm.uadd.sat.i64(i64 %outer.iv, i64 2000)
  %cap.cond = icmp ult i64 %batch.end, -1
  %inner.limit = select i1 %cap.cond, i64 %batch.end, i64 -1
  br label %uexclbad.inner.header

uexclbad.inner.header:
  %iv = phi i64 [ %outer.iv, %uexclbad.inner.entry ], [ %iv.next, %uexclbad.inner.latch ]
  %cond = icmp ult i64 %iv, %inner.limit
  br i1 %cond, label %uexclbad.inner.body, label %uexclbad.outer.latch

uexclbad.inner.body:
  br label %uexclbad.inner.latch

uexclbad.inner.latch:
  %iv.next = add i64 %iv, 2
  br label %uexclbad.inner.header, !strip-mined !0

uexclbad.outer.latch:
  %outer.iv.next = phi i64 [ %iv, %uexclbad.inner.header ]
  call hotspotcc void @jeandle.safepoint_poll(), !poll-coverage !0
  br label %uexclbad.outer

uexclbad.exit:
  ret void
}

define void @forged_marker_exclusive_step2_body_guard_only(i64 %n) {
entry:
  br label %bodyguard.outer

bodyguard.outer:
  %outer.iv = phi i64 [ 0, %entry ], [ %outer.iv.next, %bodyguard.outer.latch ]
  %outer.cond = icmp slt i64 %outer.iv, %n
  br i1 %outer.cond, label %bodyguard.inner.entry, label %bodyguard.exit

bodyguard.inner.entry:
  %batch.end = call i64 @llvm.sadd.sat.i64(i64 %outer.iv, i64 2000)
  %cap.cond = icmp slt i64 %batch.end, %n
  %inner.limit = select i1 %cap.cond, i64 %batch.end, i64 %n
  br label %bodyguard.inner.header

bodyguard.inner.header:
  %iv = phi i64 [ %outer.iv, %bodyguard.inner.entry ], [ %iv.next, %bodyguard.inner.latch ]
  %cond = icmp slt i64 %iv, %inner.limit
  br i1 %cond, label %bodyguard.inner.body, label %bodyguard.outer.latch

bodyguard.inner.body:
  %inrange = icmp slt i64 %iv, 9223372036854775806
  call void @llvm.assume(i1 %inrange)
  br label %bodyguard.inner.latch

bodyguard.inner.latch:
  %iv.next = add i64 %iv, 2
  br label %bodyguard.inner.header, !strip-mined !0

bodyguard.outer.latch:
  %outer.iv.next = phi i64 [ %iv, %bodyguard.inner.header ]
  call hotspotcc void @jeandle.safepoint_poll(), !poll-coverage !0
  br label %bodyguard.outer

bodyguard.exit:
  ret void
}

define void @forged_marker_side_exit_inside_outer(i64 %n, i1 %take) {
entry:
  br label %sidebad.outer

sidebad.outer:
  %outer.iv = phi i64 [ 0, %entry ], [ %outer.iv.next, %sidebad.outer.latch ]
  %outer.cond = icmp slt i64 %outer.iv, %n
  br i1 %outer.cond, label %sidebad.inner.entry, label %sidebad.exit

sidebad.inner.entry:
  %batch.end = call i64 @llvm.sadd.sat.i64(i64 %outer.iv, i64 1000)
  %cap.cond = icmp slt i64 %batch.end, %n
  %inner.limit = select i1 %cap.cond, i64 %batch.end, i64 %n
  br label %sidebad.inner.header

sidebad.inner.header:
  %iv = phi i64 [ %outer.iv, %sidebad.inner.entry ], [ %iv.next, %sidebad.inner.latch ]
  %cond = icmp slt i64 %iv, %inner.limit
  br i1 %cond, label %sidebad.inner.body, label %sidebad.outer.latch

sidebad.inner.body:
  br i1 %take, label %sidebad.inside.outer, label %sidebad.inner.latch

sidebad.inner.latch:
  %iv.next = add i64 %iv, 1
  br label %sidebad.inner.header, !strip-mined !0

sidebad.inside.outer:
  br label %sidebad.outer.latch

sidebad.outer.latch:
  %outer.iv.next = phi i64 [ %iv, %sidebad.inner.header ], [ %iv, %sidebad.inside.outer ]
  call hotspotcc void @jeandle.safepoint_poll(), !poll-coverage !0
  br label %sidebad.outer

sidebad.exit:
  ret void
}

!java-method-compilation = !{}
!0 = !{}

; CHECK-DAG: SafepointCoverageVerifier: loop with header 'forged.header' in function 'forged_marker_no_outer' has no dominating safepoint poll and no provable trip bound
; CHECK-DAG: SafepointCoverageVerifier: loop with header 'nopoll.inner.header' in function 'forged_marker_no_outer_poll' has no dominating safepoint poll and no provable trip bound
; CHECK-DAG: SafepointCoverageVerifier: loop with header 'bad.inner.header' in function 'forged_marker_bad_batch' has no dominating safepoint poll and no provable trip bound
; CHECK-DAG: SafepointCoverageVerifier: loop with header 'badstart.inner.header' in function 'forged_marker_bad_batch_start' has no dominating safepoint poll and no provable trip bound
; CHECK-DAG: SafepointCoverageVerifier: loop with header 'latchnopoll.inner.header' in function 'forged_latch_marker_no_outer_poll' has no dominating safepoint poll and no provable trip bound
; CHECK-DAG: SafepointCoverageVerifier: loop with header 'latchbad.inner.header' in function 'forged_latch_marker_bad_resume' has no dominating safepoint poll and no provable trip bound
; CHECK-DAG: SafepointCoverageVerifier: loop with header 'nebad.inner.header' in function 'forged_marker_ne_bad_batch' has no dominating safepoint poll and no provable trip bound
; CHECK-DAG: SafepointCoverageVerifier: loop with header 'inclbad.inner.header' in function 'forged_marker_inclusive_bad_margin' has no dominating safepoint poll and no provable trip bound
; CHECK-DAG: SafepointCoverageVerifier: loop with header 'wrongguard.inner.header' in function 'forged_marker_inclusive_wrong_guard' has no dominating safepoint poll and no provable trip bound
; CHECK-DAG: SafepointCoverageVerifier: loop with header 'unstable.inner.header' in function 'forged_marker_inclusive_unstable_limit' has no dominating safepoint poll and no provable trip bound
; CHECK-DAG: SafepointCoverageVerifier: loop with header 'maskedlimit.inner.header' in function 'forged_marker_inclusive_masked_undef_limit' has no dominating safepoint poll and no provable trip bound
; CHECK-DAG: SafepointCoverageVerifier: loop with header 'maskedstart.inner.header' in function 'forged_marker_inclusive_masked_undef_start' has no dominating safepoint poll and no provable trip bound
; CHECK-DAG: SafepointCoverageVerifier: loop with header 'uinclbad.inner.header' in function 'forged_marker_uinclusive_bad_margin' has no dominating safepoint poll and no provable trip bound
; CHECK-DAG: SafepointCoverageVerifier: loop with header 'uexclbad.inner.header' in function 'forged_marker_uexclusive_step2_wrap' has no dominating safepoint poll and no provable trip bound
; CHECK-DAG: SafepointCoverageVerifier: loop with header 'bodyguard.inner.header' in function 'forged_marker_exclusive_step2_body_guard_only' has no dominating safepoint poll and no provable trip bound
; CHECK-DAG: SafepointCoverageVerifier: loop with header 'sidebad.inner.header' in function 'forged_marker_side_exit_inside_outer' has no dominating safepoint poll and no provable trip bound

; ABORT: Jeandle safepoint coverage verification failed
