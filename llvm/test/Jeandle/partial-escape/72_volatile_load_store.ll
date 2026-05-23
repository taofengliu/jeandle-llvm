; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; volatile load/store on a virtual that never escapes. Volatile is a
; semantic restriction on optimizer reordering, but it does not change escape
; semantics for an object that the analyzer can prove never leaks. The
; allocation, the volatile store, and the volatile load should all be
; eliminated; the load resolves to the stored constant 7.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @use(i32)
declare i32 @__gxx_personality_v0(...)

define void @test_volatile_no_escape() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %n unwind label %u
n:
  %s = getelementptr inbounds i8, ptr addrspace(1) %o, i64 8
  store volatile i32 7, ptr addrspace(1) %s, align 4
  %v = load volatile i32, ptr addrspace(1) %s, align 4
  call void @use(i32 %v)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @test_volatile_no_escape
; CHECK-NOT: jeandle.new_instance
; CHECK-NOT: store volatile
; CHECK-NOT: load volatile
; CHECK: call void @use(i32 7)

!java-method-compilation = !{}
