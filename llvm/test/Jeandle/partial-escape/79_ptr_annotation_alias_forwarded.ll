; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; @llvm.ptr.annotation returns its first argument (the annotated pointer) — it
; is pointer-IDENTITY-preserving, exactly like launder/strip.invariant.group.
; When the annotated pointer is then used to access a virtual object's field,
; PEA must forward the alias so the store/load is tracked on the virtual and
; the object can be scalar-replaced. Before the fix, ptr.annotation was treated
; as a no-op, the annotated pointer was NOT aliased to the virtual, the field
; access through it was untracked, and the (still NeverEscapes) object was
; eliminated while the live load read through a poison pointer.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare ptr addrspace(1) @llvm.ptr.annotation.p1.p0(ptr addrspace(1), ptr, ptr, i32, ptr)
declare i32 @__gxx_personality_v0(...)

@.str.annotation = private constant [4 x i8] c"foo\00"
@.str.fileinfo  = private constant [4 x i8] c"bar\00"

define i32 @test_ptr_annotation_alias_forward() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %n unwind label %u
n:
  ; Annotate %o; the result is %o's identity. Field access then goes through
  ; the annotated pointer.
  %ann = call ptr addrspace(1) @llvm.ptr.annotation.p1.p0(
                ptr addrspace(1) %o,
                ptr @.str.annotation, ptr @.str.fileinfo, i32 0, ptr null)
  %s = getelementptr inbounds i8, ptr addrspace(1) %ann, i64 8
  store i32 5, ptr addrspace(1) %s
  %v = load i32, ptr addrspace(1) %s
  ret i32 %v
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; The allocation is eliminated and the field load forwards to the stored
; constant 5 — proof the annotation forwarded the virtual alias.
; CHECK-LABEL: define i32 @test_ptr_annotation_alias_forward
; CHECK-NOT: jeandle.new_instance
; CHECK-NOT: store
; CHECK-NOT: load
; CHECK: ret i32 5

!java-method-compilation = !{}
