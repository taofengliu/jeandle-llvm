; Synthetic IR exercising all four safepoint categories. Textual shapes only;
; the counting script does not require verifier-clean IR.

declare hotspotcc void @jeandle.safepoint_poll()
declare hotspotcc void @safepoint_handler(ptr)
declare token @llvm.experimental.gc.statepoint.p0(i64, i32, ptr, i32, i32, ...)
declare hotspotcc void @some.java.method()

define void @f(ptr %thread) gc "example" {
entry:
  call hotspotcc void @jeandle.safepoint_poll()
  call hotspotcc void @jeandle.safepoint_poll()
  call hotspotcc void @safepoint_handler(ptr %thread)
  %sp1 = call token (i64, i32, ptr, i32, i32, ...) @llvm.experimental.gc.statepoint.p0(i64 0, i32 0, ptr elementtype(void (ptr)) @safepoint_handler, i32 1, i32 0, ptr %thread)
  %sp2 = call token (i64, i32, ptr, i32, i32, ...) @llvm.experimental.gc.statepoint.p0(i64 0, i32 0, ptr elementtype(void ()) @some.java.method, i32 0, i32 0)
  %sp3 = call token (i64, i32, ptr, i32, i32, ...) @llvm.experimental.gc.statepoint.p0(i64 0, i32 0, ptr elementtype(void ()) @some.java.method, i32 0, i32 0)
  ret void
}
