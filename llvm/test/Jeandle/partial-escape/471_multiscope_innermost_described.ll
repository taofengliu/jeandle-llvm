; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; Multi-scope bundle, innermost-only reference (review §3 #7 companion): a
; VO referenced only INSIDE the innermost scope is still described normally
; — the guard only excludes OUTER-scope references. Two-scope bundle with
; %o in the INNERMOST scope's locals; the root scope has no VO references.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(i32)
declare i32 @__gxx_personality_v0(...)

define void @multiscope_inner(i32 %x) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 100 to ptr), i32 16)
       to label %n1 unwind label %u
n1:
  %of = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  store atomic i32 %x, ptr addrspace(1) %of unordered, align 4
  ; Two-scope bundle: ROOT scope (bci 5) has one i32 local; the INNERMOST
  ; scope (bci 9) references %o in its locals.
  call void @sink(i32 %x)
       [ "deopt"(i32 5, i32 5, i64 10, i32 %x,
                 i64 262147, i64 777,
                 i64 1, i32 9, i32 9, i64 12, ptr addrspace(1) %o) ]
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; %o is NeverEscapes (only in the bundle): eliminated and described in the
; innermost scope. The descriptor is inserted right after the innermost BCI
; pair: header (vo_id=0, ScalarValueType, T_OBJECT): (0<<32)|(4<<16)|12 =
; 262156; field (offset 8, LocalType/T_INT): (8<<32)|10 = 34359738378 -> %x;
; the locals slot becomes a VORefLocalType: (0<<32)|(8<<16)|12 = 524300,
; then i32 0.
; CHECK-LABEL: define void @multiscope_inner(
; CHECK-NOT: invoke hotspotcc ptr addrspace(1) @jeandle.new_instance
; CHECK: call void @sink(i32 %x)
; CHECK-SAME: [ "deopt"(i32 5, i32 5, i64 10, i32 %x,
; CHECK-SAME: i64 262147, i64 777,
; CHECK-SAME: i64 1, i32 9, i32 9, i64 262156, i64 100, i32 1, i64 34359738378, i32 %x, i64 524300, i32 0) ]
; CHECK-NOT: poison

!java-method-compilation = !{}
