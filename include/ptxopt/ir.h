#pragma once
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace ptx {

struct ParseError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

enum class OpKind { Reg, Pair, Vec, Mem, Imm, Sym, Special };

struct Operand {
  OpKind kind = OpKind::Imm;
  std::string text;               // Imm: literal. Sym/Special: name. Mem: offset incl. sign ("+8")
  std::vector<std::string> regs;  // Reg:1  Pair:2  Vec:n  Mem:[base] when register based
  std::string sym;                // Mem with a symbolic base, e.g. kernel_param_0
  std::string str() const;
};

Operand makeReg(const std::string& name);
Operand makeImm(const std::string& text);

struct Instr {
  std::string guard;  // predicate register, empty if unguarded
  bool guardNeg = false;
  std::string op;                 // base opcode, e.g. "add"
  std::vector<std::string> mods;  // modifiers, e.g. {"s32"}
  std::vector<Operand> ops;
  int line = 0;
  // Fallback for statements the operand parser could not understand. They are
  // kept verbatim and treated as opaque (never optimised, never removed).
  bool raw = false;
  std::string rawText;
  std::vector<std::string> rawRegs;

  std::string full() const;  // "add.s32"
  bool hasMod(const std::string& m) const;
  std::string typeMod() const;                // last modifier that is a PTX type
  std::vector<std::string> typeMods() const;  // all type modifiers in order
  std::string str() const;                    // printable text without ';'
};

// Declared registers (.reg .b32 %r<6>; .reg .pred %p1;)
struct RegDecls {
  struct Range {
    std::string prefix;
    int count;
    std::string type;
  };
  std::vector<Range> ranges;
  std::map<std::string, std::string> single;
  bool declared(const std::string& name) const;
  std::string typeOf(const std::string& name) const;  // "" if unknown
};

struct Item {
  enum class Kind { Label, Inst, Raw };
  Kind kind = Kind::Raw;
  std::string text;  // label name or raw directive text
  Instr ins;
  bool dead = false;  // marked for removal; compacted at the end of a pass
  int line = 0;
};

struct Function {
  std::string name;
  bool isEntry = false;
  bool verbatim = false;  // not analysed/optimised, printed as in the input
  std::string verbatimReason;
  std::string rawText;  // full original text (used when verbatim)
  std::string header;   // text from linkage up to (not including) '{'
  std::vector<std::string> decls;  // '.reg' lines
  RegDecls regs;
  std::vector<Item> items;
  int line = 0;
};

struct TopItem {
  bool isFunc = false;
  std::string raw;
  size_t func = 0;
};

struct Module {
  std::vector<TopItem> top;
  std::vector<Function> funcs;
};

std::string printFunction(const Function& f);
std::string printModule(const Module& m);
int countInstrs(const Function& f);
void compact(Function& f);  // erase items flagged dead

}  // namespace ptx
