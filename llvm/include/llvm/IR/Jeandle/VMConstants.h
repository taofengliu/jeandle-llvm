//===-- VMConstants.h - Jeandle VM layout constants ------------*- C++ -*-===//
//
// Header offsets and per-element constants the LLVM-side PEA needs to reason
// about Java heap objects.  The values are immutable for the lifetime of the
// VM and are populated once by the HotSpot side at LLVM init time via
// setVMConstants().  Defaults below match a typical 64-bit build with
// compressed klass pointers; the HotSpot side overrides them.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_IR_JEANDLE_VMCONSTANTS_H
#define LLVM_IR_JEANDLE_VMCONSTANTS_H

#include <cstdint>

namespace llvm {

class Type;

namespace jeandle {

// BasicType IDs mirroring HotSpot's enum BasicType in oops/typeArrayKlass.hpp.
// Used as index into per-element-type tables below.
enum class JBasicType : uint8_t {
  Boolean = 0, Byte = 1, Char = 2, Short = 3,
  Int = 4,     Long = 5, Float = 6, Double = 7,
  Object = 8,  Count = 9
};

struct VMConstants {
  // oopDesc layout (in bytes).
  int64_t MarkWordOffset = 0;
  int64_t KlassOffset = 8;            // 8 with compressed klass; HotSpot may override.
  int64_t DefaultMarkWord = 1;        // markWord::prototype() — biased lock bit cleared.

  // arrayOopDesc layout.
  int64_t ArrayLengthOffset = 12;     // 12 with compressed klass; 16 without.

  // Per-element-type base offsets. Indexed by JBasicType.
  int64_t ArrayBaseOffset[(unsigned)JBasicType::Count] = {
      /*Boolean*/16, /*Byte*/16, /*Char*/16, /*Short*/16,
      /*Int*/16,     /*Long*/16, /*Float*/16, /*Double*/16,
      /*Object*/16,
  };

  // Per-element size in bytes. Indexed by JBasicType.
  uint64_t ElementSize[(unsigned)JBasicType::Count] = {
      /*Boolean*/1, /*Byte*/1, /*Char*/2, /*Short*/2,
      /*Int*/4,     /*Long*/8, /*Float*/4, /*Double*/8,
      /*Object*/4,                          // compressed oop default
  };

  bool UseCompressedClassPointers = true;
  bool UseCompressedOops = true;

  // Convenience accessors.
  int64_t arrayLengthOffset() const { return ArrayLengthOffset; }
  int64_t klassOffset() const { return KlassOffset; }
  int64_t markWordOffset() const { return MarkWordOffset; }
  int64_t defaultMarkWord() const { return DefaultMarkWord; }

  // Returns the array base offset for an element type. Returns -1 if Ty is not
  // a recognised Jeandle element type.
  int64_t arrayBaseOffsetFor(JBasicType Kind) const {
    return ArrayBaseOffset[(unsigned)Kind];
  }
  uint64_t elementSizeFor(JBasicType Kind) const {
    return ElementSize[(unsigned)Kind];
  }

  // Best-effort mapping from an LLVM Type * to JBasicType. nullptr-safe.
  // Returns JBasicType::Count if Ty does not map to a Java basic type we model.
  static JBasicType classifyType(Type *Ty);
};

// Global accessor. HotSpot side will call setVMConstants(...) at init.  Until
// then, the defaults above apply (suitable for in-process unit tests / lit
// tests).
const VMConstants &getVMConstants();
void setVMConstants(VMConstants C);

} // namespace jeandle
} // namespace llvm

#endif
