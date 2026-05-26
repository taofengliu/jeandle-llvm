; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" -jeandle-vm-callback-log=%S/Inputs/212_arraystorecheck_int_array.cblog %s | FileCheck %s

; Virtual int[] (primitive array). ArrayElementKlass returns 0 (the
; "primitive" sentinel), so foldArrayStoreCheck unconditionally elides the
; check call. The int[] alloc is otherwise unused, so the alloc and the
; check both disappear.

declare hotspotcc ptr addrspace(1) @jeandle.newarray(ptr, i32)
declare hotspotcc i1 @jeandle.array_store_check(ptr addrspace(1), ptr addrspace(1))

declare i32 @__gxx_personality_v0(...)

define i1 @test_storecheck_prim(ptr addrspace(1) %v) gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %arr = invoke hotspotcc ptr addrspace(1) @jeandle.newarray(
            ptr inttoptr (i64 12345 to ptr), i32 4)
         to label %n unwind label %u
n:
  %r = call hotspotcc i1 @jeandle.array_store_check(ptr addrspace(1) %v,
                                                    ptr addrspace(1) %arr)
  ret i1 %r
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define i1 @test_storecheck_prim
; CHECK-NOT: jeandle.newarray
; CHECK-NOT: jeandle.array_store_check
; CHECK: ret i1 true

!java-method-compilation = !{}
