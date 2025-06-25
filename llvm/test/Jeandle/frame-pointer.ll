; RUN: llc -O3 -mtriple=x86_64-linux-gnu < %s | FileCheck %s

define hotspotcc i32 @"Main_testFramePointer_(II)I"(i32 %0, i32 %1) local_unnamed_addr #0 gc "hotspotgc" {
; CHECK: pushq %rbp
; CHECK-NOT: push $0x9
; CHECK-NOT: push $0x8
; CHECK-NOT: push $0x7
; CHECK: popq	%rbp
entry:
  %statepoint_token = tail call hotspotcc token (i64, i32, ptr, i32, i32, ...) @llvm.experimental.gc.statepoint.p0(i64 0, i32 5, ptr elementtype(i32 (i32, i32, i32, i32, i32, i32, i32, i32, i32)) @"Main_add_(IIIIIIIII)I", i32 9, i32 0, i32 1, i32 2, i32 3, i32 4, i32 5, i32 6, i32 7, i32 8, i32 9, i32 0, i32 0)
  %2 = call i32 @llvm.experimental.gc.result.i32(token %statepoint_token)
  ret i32 %2
}

declare hotspotcc i32 @"Main_add_(IIIIIIIII)I"(i32, i32, i32, i32, i32, i32, i32, i32, i32) local_unnamed_addr #1 gc "hotspotgc"

declare token @llvm.experimental.gc.statepoint.p0(i64 immarg, i32 immarg, ptr, i32 immarg, i32 immarg, ...)

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(none)
declare i32 @llvm.experimental.gc.result.i32(token) #2

attributes #0 = { "java-method" "use-compressed-oops" }
attributes #1 = { "use-compressed-oops" }
attributes #2 = { nocallback nofree nosync nounwind willreturn memory(none) }
