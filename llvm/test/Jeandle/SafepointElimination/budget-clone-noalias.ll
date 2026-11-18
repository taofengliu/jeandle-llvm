; RUN: opt -verify-each -jeandle-loop-strip-mining-iter=4 -passes='loop-simplify,lcssa,safepoint-budget-strip-mining,verify<jeandle-safepoint-coverage>' -S %s | FileCheck %s

declare hotspotcc void @jeandle.safepoint_poll()
declare void @llvm.experimental.noalias.scope.decl(metadata)

define void @local_scope(ptr %out, ptr %flag) "java-method" {
entry: br label %header
header:
  call void @llvm.experimental.noalias.scope.decl(metadata !0)
  call hotspotcc void @jeandle.safepoint_poll() ["deopt"()]
  store i32 1, ptr %out, !alias.scope !0
  %more = load volatile i1, ptr %flag
  br i1 %more, label %header, label %exit
exit: ret void
}

; The two sequential pieces must have distinct dynamic noalias declarations.
; CHECK-LABEL: define void @local_scope(
; CHECK: header.fast:
; CHECK: call void @llvm.experimental.noalias.scope.decl(metadata ![[FAST:[0-9]+]])
; CHECK: store i32 1, ptr %out, align 4, !alias.scope ![[FAST]]
; CHECK: header:
; CHECK: call void @llvm.experimental.noalias.scope.decl(metadata ![[ORIG:[0-9]+]])
; CHECK: call hotspotcc void @jeandle.safepoint_poll()
; CHECK: store i32 1, ptr %out, align 4, !alias.scope ![[ORIG]]
; CHECK-DAG: ![[FAST]] = !{![[FSCOPE:[0-9]+]]}
; CHECK-DAG: ![[FSCOPE]] = distinct !{![[FSCOPE]], !{{[0-9]+}}, !"scope:fast"}
; CHECK-DAG: ![[ORIG]] = !{![[OSCOPE:[0-9]+]]}
; CHECK-DAG: ![[OSCOPE]] = distinct !{![[OSCOPE]], !{{[0-9]+}}, !"scope"}

!0 = !{!1}
!1 = distinct !{!1, !2, !"scope"}
!2 = distinct !{!2, !"domain"}
!java-method-compilation = !{}
