; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; #2.2 variant: symbolic-offset GEP. %g = gep %o, %sym where %sym is a
; non-constant SSA value, so resolveFieldOffset returns nullopt. foldICmpEquality
; must NOT fold (it cannot prove the offsets equal); it returns false so the
; gate materializes %o and the icmp survives as a real compare.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @use(i1)
declare i32 @__gxx_personality_v0(...)

define void @test_icmp_eq_symbolic_offset(i64 %sym) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 32)
         to label %n unwind label %u
n:
  %g = getelementptr inbounds i8, ptr addrspace(1) %o, i64 %sym
  %c = icmp eq ptr addrspace(1) %o, %g
  call void @use(i1 %c)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @test_icmp_eq_symbolic_offset
; %o materializes and the icmp survives as a real compare (not folded).
; CHECK: invoke{{.*}}@jeandle.new_instance
; CHECK: icmp eq ptr addrspace(1)
; CHECK-NOT: call{{.*}}@use(i1 true)

!java-method-compilation = !{}
