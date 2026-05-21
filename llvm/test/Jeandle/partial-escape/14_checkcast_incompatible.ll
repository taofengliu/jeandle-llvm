; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" -jeandle-vm-callback-log=%S/Inputs/14_checkcast_incompatible.cblog %s | FileCheck %s

; PEA: jeandle.check_cast against a klass incompatible with the
; virtual's exact allocation klass folds to constant false. Virtual objects
; always have an exact klass, so areKlassesIncompatible can complete the
; proof without further VM callbacks beyond IsSubtype + IsInterface.

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare hotspotcc i1 @jeandle.check_cast(ptr addrspace(0), ptr addrspace(1))

declare i32 @__gxx_personality_v0(...)

define i1 @test_checkcast_incompatible() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 5 to ptr), i32 16)
       to label %n unwind label %u
n:
  %r = call hotspotcc i1 @jeandle.check_cast(
            ptr addrspace(0) inttoptr (i64 99 to ptr),
            ptr addrspace(1) %o)
  ret i1 %r
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define i1 @test_checkcast_incompatible
; CHECK-NOT: jeandle.new_instance
; CHECK-NOT: jeandle.check_cast
; CHECK: ret i1 false

!java-method-compilation = !{}
