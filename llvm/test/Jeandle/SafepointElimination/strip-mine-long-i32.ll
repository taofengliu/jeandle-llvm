; RUN: opt -passes='safepoint-elimination<early>,safepoint-elimination<strip-mining>' -jeandle-enable-strip-mining -S < %s | FileCheck %s

; R5 (contract §4.b / §8): an i64 ("long") IV with a provable constant trip
; count that fits 32 bits (2e9 < 2^32) but exceeds the chunk budget. The
; rejected width<2^32 rule would bare-delete the poll, leaving the loop naked
; for up to ~2e9 iterations (Falcon's measured ~2.1s TTSP). Instead it must be
; STRIP-MINED: the poll relocates to the outer back-edge with coverage, it is
; not deleted.

declare hotspotcc void @jeandle.safepoint_poll()

define void @r5(ptr %a) {
entry:
  br label %h

h:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %lat ]
  %c = icmp slt i64 %iv, 2000000000
  br i1 %c, label %b, label %x

b:
  %p = getelementptr inbounds i32, ptr %a, i64 %iv
  store i32 0, ptr %p
  %iv.next = add i64 %iv, 1
  call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i32 5, i64 %iv.next) ]
  br label %lat

lat:
  br label %h

x:
  ret void
}

!java-method-compilation = !{}

; Inner body is poll-free — the poll was relocated, not bare-deleted.
; CHECK-LABEL: @r5(
; CHECK:       b:
; CHECK-NOT:     call hotspotcc void @jeandle.safepoint_poll
; CHECK:         br label %lat

; The outer loop walks the full constant bound in batches of N.
; CHECK:       h.outer:
; CHECK:         icmp slt i64 %outer.iv, 2000000000
; CHECK:       h.outer.inner.entry:
; CHECK:         call i64 @llvm.sadd.sat.i64(i64 %outer.iv, i64 1000)

; The poll survives on the outer back-edge with coverage — the load-bearing R5
; proof that the bounded i64 loop was strip-mined, not bare-deleted.
; CHECK:       h.outer.latch:
; CHECK:         call hotspotcc void @jeandle.safepoint_poll() [ "deopt"(i32 5, i64 %outer.iv.next) ]{{.*}}!poll-coverage
