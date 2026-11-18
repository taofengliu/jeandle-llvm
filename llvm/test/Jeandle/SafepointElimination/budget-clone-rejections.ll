; RUN: opt -verify-each -jeandle-loop-strip-mining-iter=4 -jeandle-safepoint-max-function-clone-instructions=9 -passes='loop-simplify,lcssa,safepoint-budget-strip-mining,safepoint-budget-strip-mining' -S %s | FileCheck %s --check-prefix=LIMIT
; RUN: opt -verify-each -jeandle-loop-strip-mining-iter=4 -passes='loop-simplify,lcssa,safepoint-budget-strip-mining,safepoint-poll-elimination<after-strip-mining>,verify<jeandle-safepoint-coverage>' -S %s | FileCheck %s
; RUN: opt -verify-each -jeandle-loop-strip-mining-iter=4 -jeandle-safepoint-max-function-clone-instructions=9 -passes='loop-simplify,lcssa,safepoint-budget-strip-mining' -S %s | FileCheck %s --check-prefix=LIMIT

declare hotspotcc void @jeandle.safepoint_poll()
declare void @convergent_call() convergent
declare void @noduplicate_call() noduplicate
declare i32 @stateful_call(i32)

define void @convergence(ptr %out, ptr %flag, i32 %n) "java-method" {
entry: br label %header
header:
  call void @convergent_call()
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 %n)]
  store volatile i32 %n, ptr %out
  %more = load volatile i1, ptr %flag
  br i1 %more, label %header, label %exit
exit: ret void
}
; CHECK-LABEL: define void @convergence(
; CHECK-NOT: batch.outer
; CHECK: header:
; CHECK: call hotspotcc void @jeandle.safepoint_poll()
; CHECK: ret void

define void @noduplicate(ptr %out, ptr %flag, i32 %n) "java-method" {
entry: br label %header
header:
  call void @noduplicate_call()
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 %n)]
  store volatile i32 %n, ptr %out
  %more = load volatile i1, ptr %flag
  br i1 %more, label %header, label %exit
exit: ret void
}
; CHECK-LABEL: define void @noduplicate(
; CHECK-NOT: batch.outer
; CHECK: header:
; CHECK: call hotspotcc void @jeandle.safepoint_poll()
; CHECK: ret void

define void @identity(ptr %out, ptr %flag, i32 %n) "java-method" {
entry: br label %header
header:
  call i32 @stateful_call(i32 %n) "statepoint-id"="1234"
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 %n)]
  store volatile i32 %n, ptr %out
  %more = load volatile i1, ptr %flag
  br i1 %more, label %header, label %exit
exit: ret void
}
; CHECK-LABEL: define void @identity(
; CHECK-NOT: batch.outer
; CHECK: header:
; CHECK: call hotspotcc void @jeandle.safepoint_poll()
; CHECK: ret void

define void @large(ptr %out, ptr %flag, i32 %n) "java-method" {
entry: br label %header
header:

  call hotspotcc void @jeandle.safepoint_poll() ["deopt"(i32 %n)]
  %x0 = add i32 %n, 1
  %x1 = add i32 %x0, 1
  %x2 = add i32 %x1, 1
  %x3 = add i32 %x2, 1
  %x4 = add i32 %x3, 1
  %x5 = add i32 %x4, 1
  %x6 = add i32 %x5, 1
  %x7 = add i32 %x6, 1
  %x8 = add i32 %x7, 1
  %x9 = add i32 %x8, 1
  %x10 = add i32 %x9, 1
  %x11 = add i32 %x10, 1
  %x12 = add i32 %x11, 1
  %x13 = add i32 %x12, 1
  %x14 = add i32 %x13, 1
  %x15 = add i32 %x14, 1
  %x16 = add i32 %x15, 1
  %x17 = add i32 %x16, 1
  %x18 = add i32 %x17, 1
  %x19 = add i32 %x18, 1
  %x20 = add i32 %x19, 1
  %x21 = add i32 %x20, 1
  %x22 = add i32 %x21, 1
  %x23 = add i32 %x22, 1
  %x24 = add i32 %x23, 1
  %x25 = add i32 %x24, 1
  %x26 = add i32 %x25, 1
  %x27 = add i32 %x26, 1
  %x28 = add i32 %x27, 1
  %x29 = add i32 %x28, 1
  %x30 = add i32 %x29, 1
  %x31 = add i32 %x30, 1
  %x32 = add i32 %x31, 1
  %x33 = add i32 %x32, 1
  %x34 = add i32 %x33, 1
  %x35 = add i32 %x34, 1
  %x36 = add i32 %x35, 1
  %x37 = add i32 %x36, 1
  %x38 = add i32 %x37, 1
  %x39 = add i32 %x38, 1
  %x40 = add i32 %x39, 1
  %x41 = add i32 %x40, 1
  %x42 = add i32 %x41, 1
  %x43 = add i32 %x42, 1
  %x44 = add i32 %x43, 1
  %x45 = add i32 %x44, 1
  %x46 = add i32 %x45, 1
  %x47 = add i32 %x46, 1
  %x48 = add i32 %x47, 1
  %x49 = add i32 %x48, 1
  %x50 = add i32 %x49, 1
  %x51 = add i32 %x50, 1
  %x52 = add i32 %x51, 1
  %x53 = add i32 %x52, 1
  %x54 = add i32 %x53, 1
  %x55 = add i32 %x54, 1
  %x56 = add i32 %x55, 1
  %x57 = add i32 %x56, 1
  %x58 = add i32 %x57, 1
  %x59 = add i32 %x58, 1
  %x60 = add i32 %x59, 1
  %x61 = add i32 %x60, 1
  %x62 = add i32 %x61, 1
  %x63 = add i32 %x62, 1
  %x64 = add i32 %x63, 1
  %x65 = add i32 %x64, 1
  %x66 = add i32 %x65, 1
  %x67 = add i32 %x66, 1
  %x68 = add i32 %x67, 1
  %x69 = add i32 %x68, 1
  %x70 = add i32 %x69, 1
  %x71 = add i32 %x70, 1
  %x72 = add i32 %x71, 1
  %x73 = add i32 %x72, 1
  %x74 = add i32 %x73, 1
  %x75 = add i32 %x74, 1
  %x76 = add i32 %x75, 1
  %x77 = add i32 %x76, 1
  %x78 = add i32 %x77, 1
  %x79 = add i32 %x78, 1
  %x80 = add i32 %x79, 1
  %x81 = add i32 %x80, 1
  %x82 = add i32 %x81, 1
  %x83 = add i32 %x82, 1
  %x84 = add i32 %x83, 1
  %x85 = add i32 %x84, 1
  %x86 = add i32 %x85, 1
  %x87 = add i32 %x86, 1
  %x88 = add i32 %x87, 1
  %x89 = add i32 %x88, 1
  %x90 = add i32 %x89, 1
  %x91 = add i32 %x90, 1
  %x92 = add i32 %x91, 1
  %x93 = add i32 %x92, 1
  %x94 = add i32 %x93, 1
  %x95 = add i32 %x94, 1
  %x96 = add i32 %x95, 1
  %x97 = add i32 %x96, 1
  %x98 = add i32 %x97, 1
  %x99 = add i32 %x98, 1
  %x100 = add i32 %x99, 1
  %x101 = add i32 %x100, 1
  %x102 = add i32 %x101, 1
  %x103 = add i32 %x102, 1
  %x104 = add i32 %x103, 1
  %x105 = add i32 %x104, 1
  %x106 = add i32 %x105, 1
  %x107 = add i32 %x106, 1
  %x108 = add i32 %x107, 1
  %x109 = add i32 %x108, 1
  %x110 = add i32 %x109, 1
  %x111 = add i32 %x110, 1
  %x112 = add i32 %x111, 1
  %x113 = add i32 %x112, 1
  %x114 = add i32 %x113, 1
  %x115 = add i32 %x114, 1
  %x116 = add i32 %x115, 1
  %x117 = add i32 %x116, 1
  %x118 = add i32 %x117, 1
  %x119 = add i32 %x118, 1
  %x120 = add i32 %x119, 1
  %x121 = add i32 %x120, 1
  %x122 = add i32 %x121, 1
  %x123 = add i32 %x122, 1
  %x124 = add i32 %x123, 1
  %x125 = add i32 %x124, 1
  %x126 = add i32 %x125, 1
  %x127 = add i32 %x126, 1
  %x128 = add i32 %x127, 1
  %x129 = add i32 %x128, 1
  store volatile i32 %x129, ptr %out
  %more = load volatile i1, ptr %flag
  br i1 %more, label %header, label %exit
exit: ret void
}
; CHECK-LABEL: define void @large(
; CHECK-NOT: batch.outer
; CHECK: header:
; CHECK: call hotspotcc void @jeandle.safepoint_poll()
; CHECK: ret void

; Each source loop has four non-debug instructions. After two clones the
; cumulative budget cannot accommodate the third, despite analysis rebuilding.
define void @cumulative(ptr %out, ptr %flag) "java-method" {
entry: br label %one
one:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"()]
  store volatile i32 1, ptr %out
  %c1 = load volatile i1, ptr %flag
  br i1 %c1, label %one, label %between1
between1: br label %two
two:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"()]
  store volatile i32 2, ptr %out
  %c2 = load volatile i1, ptr %flag
  br i1 %c2, label %two, label %between2
between2: br label %three
three:
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"()]
  store volatile i32 3, ptr %out
  %c3 = load volatile i1, ptr %flag
  br i1 %c3, label %three, label %exit
exit: ret void
}
; CHECK-LABEL: define void @cumulative(
; CHECK-COUNT-3: batch.outer{{[0-9]*}}:
; CHECK: ret void
; LIMIT-LABEL: define void @cumulative(
; LIMIT-COUNT-2: batch.outer{{[0-9]*}}:
; LIMIT-NOT: batch.outer{{[0-9]*}}:
; LIMIT: three:
; LIMIT-NEXT: call hotspotcc void @jeandle.safepoint_poll()
; LIMIT: ret void

!java-method-compilation = !{}
