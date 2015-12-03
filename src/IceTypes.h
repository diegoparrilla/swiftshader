//===- subzero/src/IceTypes.h - Primitive ICE types -------------*- C++ -*-===//
//
//                        The Subzero Code Generator
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief Declares a few properties of the primitive types allowed in Subzero.
/// Every Subzero source file is expected to include IceTypes.h.
///
//===----------------------------------------------------------------------===//

#ifndef SUBZERO_SRC_ICETYPES_H
#define SUBZERO_SRC_ICETYPES_H

#include "IceDefs.h"
#include "IceTypes.def"

namespace Ice {

enum Type {
#define X(tag, sizeLog2, align, elts, elty, str) IceType_##tag,
  ICETYPE_TABLE
#undef X
      IceType_NUM
};

enum TargetArch {
#define X(tag, str, is_elf64, e_machine, e_flags) tag,
  TARGETARCH_TABLE
#undef X
      TargetArch_NUM
};

const char *targetArchString(TargetArch Arch);

inline Ostream &operator<<(Ostream &Stream, TargetArch Arch) {
  return Stream << targetArchString(Arch);
}

/// The list of all target instruction sets. Individual targets will map this to
/// include only what is valid for the target.
enum TargetInstructionSet {
  // Represents baseline that can be assumed for a target (usually "Begin").
  BaseInstructionSet,
  X86InstructionSet_Begin,
  X86InstructionSet_SSE2 = X86InstructionSet_Begin,
  X86InstructionSet_SSE4_1,
  X86InstructionSet_End,
  ARM32InstructionSet_Begin,
  ARM32InstructionSet_Neon = ARM32InstructionSet_Begin,
  ARM32InstructionSet_HWDivArm,
  ARM32InstructionSet_End,
};

enum OptLevel { Opt_m1, Opt_0, Opt_1, Opt_2 };

size_t typeWidthInBytes(Type Ty);
int8_t typeWidthInBytesLog2(Type Ty);
size_t typeAlignInBytes(Type Ty);
size_t typeNumElements(Type Ty);
Type typeElementType(Type Ty);
const char *typeString(Type Ty);

inline Type getPointerType() { return IceType_i32; }

bool isVectorType(Type Ty);

bool isIntegerType(Type Ty); // scalar or vector
bool isScalarIntegerType(Type Ty);
bool isVectorIntegerType(Type Ty);
bool isIntegerArithmeticType(Type Ty);

bool isFloatingType(Type Ty); // scalar or vector
bool isScalarFloatingType(Type Ty);
bool isVectorFloatingType(Type Ty);

/// Returns true if the given type can be used in a load instruction.
bool isLoadStoreType(Type Ty);

/// Returns true if the given type can be used as a parameter type in a call.
bool isCallParameterType(Type Ty);

/// Returns true if the given type can be used as the return type of a call.
inline bool isCallReturnType(Type Ty) {
  return Ty == IceType_void || isCallParameterType(Ty);
}

/// Returns type generated by applying the compare instructions (icmp and fcmp)
/// to arguments of the given type. Returns IceType_void if compare is not
/// allowed.
Type getCompareResultType(Type Ty);

/// Returns the number of bits in a scalar integer type.
SizeT getScalarIntBitWidth(Type Ty);

/// Check if a type is byte sized (slight optimization over typeWidthInBytes).
inline bool isByteSizedType(Type Ty) {
  bool result = Ty == IceType_i8 || Ty == IceType_i1;
  assert(result == (1 == typeWidthInBytes(Ty)));
  return result;
}

/// Check if Ty is byte sized and specifically i8. Assert that it's not byte
/// sized due to being an i1.
inline bool isByteSizedArithType(Type Ty) {
  assert(Ty != IceType_i1);
  return Ty == IceType_i8;
}

/// Return true if Ty is i32. This asserts that Ty is either i32 or i64.
inline bool isInt32Asserting32Or64(Type Ty) {
  bool result = Ty == IceType_i32;
  assert(result || Ty == IceType_i64);
  return result;
}

/// Return true if Ty is f32. This asserts that Ty is either f32 or f64.
inline bool isFloat32Asserting32Or64(Type Ty) {
  bool result = Ty == IceType_f32;
  assert(result || Ty == IceType_f64);
  return result;
}

template <typename StreamType>
inline StreamType &operator<<(StreamType &Str, const Type &Ty) {
  Str << typeString(Ty);
  return Str;
}

/// Models a type signature for a function.
class FuncSigType {
  FuncSigType &operator=(const FuncSigType &Ty) = delete;

public:
  using ArgListType = std::vector<Type>;

  /// Creates a function signature type with the given return type. Parameter
  /// types should be added using calls to appendArgType.
  FuncSigType() = default;
  FuncSigType(const FuncSigType &Ty) = default;

  void appendArgType(Type ArgType) { ArgList.push_back(ArgType); }

  Type getReturnType() const { return ReturnType; }
  void setReturnType(Type NewType) { ReturnType = NewType; }
  SizeT getNumArgs() const { return ArgList.size(); }
  Type getArgType(SizeT Index) const {
    assert(Index < ArgList.size());
    return ArgList[Index];
  }
  const ArgListType &getArgList() const { return ArgList; }
  void dump(Ostream &Stream) const;

private:
  /// The return type.
  Type ReturnType = IceType_void;
  /// The list of parameters.
  ArgListType ArgList;
};

inline Ostream &operator<<(Ostream &Stream, const FuncSigType &Sig) {
  Sig.dump(Stream);
  return Stream;
}

} // end of namespace Ice

#endif // SUBZERO_SRC_ICETYPES_H
