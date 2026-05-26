; RUN: opt -S -passes="require<partial-escape-analysis>,partial-escape-transform" %s | FileCheck %s

; PEA is intentionally deopt-agnostic until the Jeandle deopt refactor
; lands. This pins two invariants on the materialization transform:
;
;   (Change A) The materialization invoke must not carry a "deopt"
;   operand bundle. Without this, the bundle on the source CallBase
;   (here the escape-point sink) would carry the OrigAlloc reference
;   onto NewInv, and the global RAUW would then produce a self-reference
;   ("Only PHI nodes may reference their own value!"). Observed in the
;   wild on java.util.HashMap.newNode, java.io.DataInputStream.readUTF,
;   and java.lang.StringLatin1.replace.
;
;   (Change B) Any pre-existing "deopt" operand bundle on another
;   CallBase that references OrigAlloc must have that operand scrubbed
;   to a typed null BEFORE the global RAUW runs. Otherwise the RAUW
;   would inject NewInv into deopt bundles on sibling sinks in
;   non-dominating blocks, and a later sibling materialization would
;   inherit those cross-block references on bundle copy ("Instruction
;   does not dominate all uses!").

declare hotspotcc ptr addrspace(1) @jeandle.new_instance(ptr, i32)
declare void @sink(ptr addrspace(1))
declare i32 @__gxx_personality_v0(...)

define void @deopt_bundle_scrubbed() gc "hotspotgc" personality ptr @__gxx_personality_v0 {
entry:
  %o = invoke hotspotcc ptr addrspace(1) @jeandle.new_instance(
            ptr inttoptr (i64 12345 to ptr), i32 16)
       to label %n unwind label %u
n:
  ; Sink call escape; its "deopt" bundle references the VO. Before the
  ; fix, copying this bundle onto the new materialization invoke and
  ; then RAUW'ing OrigAlloc -> NewInv would make NewInv reference
  ; itself in its own deopt bundle.
  call void @sink(ptr addrspace(1) %o)
       [ "deopt"(i32 99, ptr addrspace(1) %o) ]
  ret void
u:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @deopt_bundle_scrubbed
;
; Change A: the materialisation invoke carries no "deopt" bundle.
; CHECK: %pea.mat = invoke {{.*}}@jeandle.new_instance(ptr {{.*}}, i32 16)
; CHECK-NEXT: to label %{{.*}} unwind label %{{.*}}
;
; Change B: the surviving sink call still has its "deopt" bundle, but
; the operand that was the OrigAlloc value is now a typed null (the VO
; slot is no longer a live SSA reference to the deleted OrigAlloc).
; CHECK: call void @sink(ptr addrspace(1) %pea.mat)
; CHECK-SAME: [ "deopt"(i32 99, ptr addrspace(1) null) ]

!java-method-compilation = !{}
