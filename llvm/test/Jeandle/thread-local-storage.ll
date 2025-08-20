; RUN: opt -S --passes=tls-pointer-rewrite %s 2>&1 | FileCheck %s

; CHECK: %0 = call i64 @llvm.read_register.i64(metadata !0)
; CHECK: %.address = add i64 1160, %0
; CHECK: %.tls = inttoptr i64 %.address to ptr addrspace(2)
; CHECK: store i64 0, ptr addrspace(2) %.tls, align 8
; CHECK: %1 = load i64, ptr addrspace(2) %.tls, align 8
; CHECK: %2 = inttoptr i64 984 to ptr addrspace(2)
; CHECK: %.int = ptrtoint ptr addrspace(2) %2 to i64
; CHECK: %.address1 = add i64 %.int, %0
; CHECK: %.tls2 = inttoptr i64 %.address1 to ptr addrspace(2)
; CHECK: store i64 %1, ptr addrspace(2) %.tls2, align 8
; CHECK: store i64 0, ptr addrspace(2) %.tls2, align 8

define hotspotcc void @thread_local_storage() {
entry:
  store i64 0, ptr addrspace(2) inttoptr (i64 1160 to ptr addrspace(2)), align 8
  %0 = load i64, ptr addrspace(2) inttoptr (i64 1160 to ptr addrspace(2)), align 8
  %1 = inttoptr i64 984 to ptr addrspace(2)
  store i64 %0, ptr addrspace(2) %1, align 8
  store i64 0, ptr addrspace(2) %1, align 8
  ret void
}

!current_thread = !{!0}

!0 = !{!"r15"}
