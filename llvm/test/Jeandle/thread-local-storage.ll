; RUN: opt -S --passes=tls-pointer-rewrite %s 2>&1 | FileCheck %s

; CHECK: %tls.base = call i64 @llvm.read_register.i64(metadata !0)
; CHECK-NEXT: %tls.base.ptr = inttoptr i64 %tls.base to ptr addrspace(2)
; CHECK-NEXT: %.tls.ptr = getelementptr inbounds i8, ptr addrspace(2) %tls.base.ptr, i64 1160
; CHECK-NEXT: store i64 0, ptr addrspace(2) %.tls.ptr, align 8
; CHECK-NEXT: %0 = load i64, ptr addrspace(2) %.tls.ptr, align 8
; CHECK-NEXT: %1 = inttoptr i64 984 to ptr addrspace(2)
; CHECK-NEXT: %.tls.offset = ptrtoint ptr addrspace(2) %1 to i64
; CHECK-NEXT: %.tls.ptr1 = getelementptr inbounds i8, ptr addrspace(2) %tls.base.ptr, i64 %.tls.offset
; CHECK-NEXT: store i64 %0, ptr addrspace(2) %.tls.ptr1, align 8
; CHECK-NEXT: store i64 0, ptr addrspace(2) %.tls.ptr1, align 8

; CHECK: br1:                                              ; preds = %entry
; CHECK-NEXT: %3 = inttoptr i64 1 to ptr addrspace(2)
; CHECK-NEXT: %.tls.offset2 = ptrtoint ptr addrspace(2) %3 to i64
; CHECK-NEXT: %.tls.ptr3 = getelementptr inbounds i8, ptr addrspace(2) %tls.base.ptr, i64 %.tls.offset2
; CHECK-NEXT: br label %return

; CHECK: br2:                                              ; preds = %entry
; CHECK-NEXT: %4 = inttoptr i64 2 to ptr addrspace(2)
; CHECK-NEXT: %.tls.offset4 = ptrtoint ptr addrspace(2) %4 to i64
; CHECK-NEXT: %.tls.ptr5 = getelementptr inbounds i8, ptr addrspace(2) %tls.base.ptr, i64 %.tls.offset4
; CHECK-NEXT: br label %return

; CHECK: return:                                           ; preds = %br2, %br1
; CHECK-NEXT:   %5 = phi ptr addrspace(2) [ %.tls.ptr3, %br1 ], [ %.tls.ptr5, %br2 ]
; CHECK-NEXT:   %6 = load i64, ptr addrspace(2) %5, align 8

define hotspotcc i64 @thread_local_storage() {
entry:
  store i64 0, ptr addrspace(2) inttoptr (i64 1160 to ptr addrspace(2)), align 8
  %0 = load i64, ptr addrspace(2) inttoptr (i64 1160 to ptr addrspace(2)), align 8
  %1 = inttoptr i64 984 to ptr addrspace(2)
  store i64 %0, ptr addrspace(2) %1, align 8
  store i64 0, ptr addrspace(2) %1, align 8
  %2 = icmp eq i64 %0, 0
  br i1 %2, label %br1, label %br2

br1:
  %3 = inttoptr i64 1 to ptr addrspace(2)
  br label %return

br2:
  %4 = inttoptr i64 2 to ptr addrspace(2)
  br label %return

return:
  %5 = phi ptr addrspace(2) [ %3, %br1 ], [ %4, %br2 ]
  %6 = load i64, ptr addrspace(2) %5, align 8
  ret i64 %6
}

!current_thread = !{!0}

!0 = !{!"r15"}
