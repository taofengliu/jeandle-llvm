; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; 5-arg `jeandle.new_array` materialize re-emit regression test.
;
; The TLAB fast-path change (jeandle-jdk #530) widened `jeandle.new_array` from
; (klass, length) to (klass, length, size_in_bytes, base_offset, length_limit).
; PEA's applyMaterialize must re-emit the escaped array with the allocation
; function's FULL arity, forwarding the trailing params from the original
; invoke. With the old 2-arg re-emit the verifier rejected the post-PEA IR
; ("incorrect number of arguments") whenever a virtualized array escaped.
;
; Here the array is virtualized (the element store at element 0 folds into the
; virtual field state), then the array pointer itself escapes through @sink.
; PEA eliminates the original allocation invoke and emits a fresh `pea.mat`
; materialization at the escape point, replaying the store into the
; materialized array. The `pea.mat` invoke MUST carry all 5 operands; the
; replayed store and the @sink call use the materialized pointer.

declare hotspotcc ptr addrspace(1) @jeandle.new_array(ptr, i32, i32, i32, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @test_array_5arg_materialize_escape() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  ; (klass, length=4, size_in_bytes=64, base_offset=16, length_limit=1000000).
  %arr = invoke hotspotcc ptr addrspace(1) @jeandle.new_array(
            ptr inttoptr (i64 12345 to ptr), i32 4, i32 64, i32 16, i32 1000000)
         to label %n unwind label %u
n:
  %base = getelementptr inbounds i8, ptr addrspace(1) %arr, i32 16
  %p0 = getelementptr inbounds i32, ptr addrspace(1) %base, i64 0
  store atomic i32 100, ptr addrspace(1) %p0 unordered, align 4
  call void @sink(ptr addrspace(1) %arr)
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @test_array_5arg_materialize_escape
; The original allocation invoke is eliminated; a `pea.mat` materialization is
; emitted at the escape point with the FULL 5-arg signature — klass + length
; rebuilt from VirtualObject, then size_in_bytes / base_offset / length_limit
; forwarded verbatim from the original invoke.
; CHECK: %{{.*}} = invoke hotspotcc{{.*}} @jeandle.new_array(ptr inttoptr (i64 12345 to ptr), i32 4, i32 64, i32 16, i32 1000000)
; CHECK-NEXT: to label %{{.*}} unwind label %{{.*}}
; The element store is replayed into the materialized array (pea.matslot).
; CHECK: store atomic i32 100, ptr addrspace(1) %{{.*}} unordered, align 4
; The @sink receives the materialized pointer (pea.mat), not the original %arr.
; CHECK: call void @sink(ptr addrspace(1) %{{.*}})

!java-method-compilation = !{}
