; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" -jeandle-vm-callback-log=%S/Inputs/272_arraycopy_from_constant_data.cblog %s | FileCheck %s

; Virtual int[] initialized via llvm.memcpy from a global
; ConstantDataArray. The per-slot loads stage as constants directly into the
; dst FieldStates; loading dst[1] folds to 200.

@.cst_ints = private unnamed_addr constant [4 x i32] [i32 100, i32 200, i32 300, i32 400]

declare hotspotcc ptr addrspace(1) @jeandle.newarray(ptr, i32)
declare void @llvm.memcpy.p1.p0.i64(ptr addrspace(1), ptr, i64, i1)

declare i32 @__gxx_personality_v0(...)

define i32 @test_memcpy_from_const() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %dst = invoke hotspotcc ptr addrspace(1) @jeandle.newarray(
            ptr inttoptr (i64 12345 to ptr), i32 4)
         to label %n unwind label %u
n:
  %dbase = getelementptr inbounds i8, ptr addrspace(1) %dst, i32 16
  call void @llvm.memcpy.p1.p0.i64(ptr addrspace(1) %dbase, ptr @.cst_ints, i64 16, i1 false)
  %d1 = getelementptr inbounds i32, ptr addrspace(1) %dbase, i64 1
  %v = load atomic i32, ptr addrspace(1) %d1 unordered, align 4
  ret i32 %v
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define i32 @test_memcpy_from_const
; CHECK-NOT: jeandle.newarray
; CHECK-NOT: llvm.memcpy
; CHECK-NOT: store
; CHECK-NOT: load
; CHECK: ret i32 200

!java-method-compilation = !{}
