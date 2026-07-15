; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; jeandle.get_class on a NON-virtual operand (a function parameter) does NOT
; fold: foldGetClass bails (resolveVirtualRef returns nullopt for a param) and
; the call survives unchanged. No spurious materialization, no crash. This
; needs no callback log because foldGetClass returns before consulting one.

declare hotspotcc ptr addrspace(1) @jeandle.get_class(ptr addrspace(1))

define ptr addrspace(1) @test_get_class_nonvirtual(ptr addrspace(1) %p) gc "hotspotgc" {
entry:
  %c = call hotspotcc ptr addrspace(1) @jeandle.get_class(ptr addrspace(1) %p)
  ret ptr addrspace(1) %c
}

; CHECK-LABEL: define ptr addrspace(1) @test_get_class_nonvirtual
; CHECK: call hotspotcc ptr addrspace(1) @jeandle.get_class
; CHECK: ret ptr addrspace(1)

!java-method-compilation = !{}
