#pragma once
#include <string>
#include <vector>

#include "ptxopt/ir.h"

namespace ptx {

struct TypeInfo {
  bool valid = false;
  int bits = 0;
  bool isSigned = false, isFloat = false, isPred = false;
};
TypeInfo parseType(const std::string& t);  // "s32" -> {32, signed}
int regWidth(const std::string& declaredType);  // 0 if unknown

// What an instruction reads / writes and whether it may be deleted.
struct InstrInfo {
  std::vector<std::string> defs;      // registers written (not killing if guarded)
  std::vector<std::string> uses;      // registers read (includes the guard)
  std::vector<std::string> clobbers;  // possibly written by opaque instructions
  bool removable = false;   // pure: may be deleted when every def is dead
  bool sideEffect = false;
  bool terminator = false;  // bra / ret / exit / trap
  bool opaque = false;      // unknown semantics: never touched by passes
  int firstUse = 1;         // index of the first source operand; -1 if opaque
};
InstrInfo infoOf(const Instr& ins);

// Type of operand `idx` (shift counts are u32, cvt uses dst/src types).
TypeInfo operandType(const Instr& ins, size_t idx);

bool isTerminator(const Instr& ins);
bool isKnownSpecial(const std::string& name);  // %tid.x, %ctaid.y, ...

// Immediate literal helpers (decimal, 0x hex, optional U suffix).
bool parseIntLiteral(const std::string& text, unsigned long long& bits);
bool isFloatLiteral(const std::string& text);

}  // namespace ptx
