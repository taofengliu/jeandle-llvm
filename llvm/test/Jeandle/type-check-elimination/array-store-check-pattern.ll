; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/array-store-check-pattern.cblog %s 2>&1 | FileCheck %s

; Test: Array store check pattern where element_klass is loaded at runtime.
; The klass argument is not a constant (it's loaded from the array header),
; so extractKlassConstant returns 0 and the check is preserved.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define void @array_store(ptr addrspace(1) nonnull %array, ptr addrspace(1) nonnull "java-klass"="7" "java-klass-exact" %element) gc "hotspotgc" {
entry:
  ; Load element_klass from array header (non-constant).
  %klass_ptr = getelementptr i8, ptr addrspace(1) %array, i64 8
  %element_klass_i64 = load i64, ptr addrspace(1) %klass_ptr
  %element_klass = inttoptr i64 %element_klass_i64 to ptr
  %check = call i1 @jeandle.check_instanceof(
    ptr addrspace(0) %element_klass,
    ptr addrspace(1) nonnull %element)
  br i1 %check, label %store, label %throw

store:
  ret void

throw:
  call void @throw_array_store_exception()
  unreachable
}

declare void @throw_array_store_exception()

; CHECK-LABEL: @array_store
; CHECK: %check = call i1 @jeandle.check_instanceof(ptr %element_klass, ptr addrspace(1) nonnull %element)

!java-method-compilation = !{}
