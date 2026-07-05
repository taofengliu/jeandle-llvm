; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; getOrCreateFieldIndex unknown-size type (review #3.2). For a non-pointer
; field type, ByteSize is derived from Type::getPrimitiveSizeInBits(), which
; returns 0 for aggregate types (struct/array — and scalable vectors). Before
; the fix the `assert(Bits > 0)` fired in debug builds (opt crash); in release a
; zero-byte field was silently inserted. After the fix the function
; conservatively returns -1, processStore bails keeping the operands real, and
; the object materializes (no crash). Fixed vector types have a known primitive
; size and are handled normally, so the trigger here is a struct value.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare i32 @__gxx_personality_v0(...)

define void @test_struct_field_escape() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 48)
         to label %n unwind label %u
n:
  %slot = getelementptr inbounds i8, ptr addrspace(1) %o, i64 16
  store { i32, i32 } zeroinitializer, ptr addrspace(1) %slot, align 4
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @test_struct_field_escape
; The object survives (getOrCreateFieldIndex returned -1 -> bail -> real).
; CHECK: invoke{{.*}}@jeandle.new_instance
; CHECK: store { i32, i32 }

!java-method-compilation = !{}
