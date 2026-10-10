#include "ptxopt/semantics.h"

#include <algorithm>
#include <cctype>
#include <set>

#include "ptxopt/util.h"

namespace ptx {

TypeInfo parseType(const std::string& t) {
  TypeInfo r;
  if (t == "pred") {
    r.valid = r.isPred = true;
    r.bits = 1;
    return r;
  }
  if (t.size() < 2 || t.size() > 3) return r;
  for (size_t i = 1; i < t.size(); ++i)
    if (!std::isdigit(static_cast<unsigned char>(t[i]))) return r;
  int bits = std::stoi(t.substr(1));
  char c = t[0];
  if ((c == 's' || c == 'u' || c == 'b') && (bits == 8 || bits == 16 || bits == 32 || bits == 64)) {
    r.valid = true;
    r.bits = bits;
    r.isSigned = (c == 's');
  } else if (c == 'f' && (bits == 16 || bits == 32 || bits == 64)) {
    r.valid = r.isFloat = true;
    r.bits = bits;
  }
  return r;
}

int regWidth(const std::string& t) {
  TypeInfo ti = parseType(t);
  return ti.valid ? ti.bits : 0;
}

namespace {
const std::set<std::string> kPure = {
    "mov", "add", "sub", "mul", "mad", "fma", "div", "rem", "abs", "neg", "min", "max", "and",
    "or", "xor", "not", "cnot", "shl", "shr", "cvt", "cvta", "selp", "setp", "set", "slct", "rcp",
    "sqrt", "rsqrt", "sin", "cos", "lg2", "ex2", "tanh", "popc", "clz", "brev", "bfe", "bfi",
    "bfind", "prmt", "lop3", "mul24", "mad24", "sad", "testp", "copysign", "dp4a", "dp2a",
    "isspacep", "szext", "fns"};
const std::set<std::string> kEffectDef = {"atom", "shfl", "vote", "match", "redux", "activemask",
                                          "ldmatrix", "mma", "wmma", "elect", "addc", "subc", "madc"};
const std::set<std::string> kEffectNoDef = {
    "st", "red", "membar", "fence", "prefetch", "prefetchu", "barrier", "stmatrix", "bra",
    "ret", "exit", "trap", "brkpt", "pmevent", "discard", "applypriority", "nanosleep"};

void addRegs(const Operand& o, std::vector<std::string>& out) {
  if (o.kind == OpKind::Reg || o.kind == OpKind::Pair || o.kind == OpKind::Vec || o.kind == OpKind::Mem)
    for (const auto& r : o.regs) out.push_back(r);
}
}  // namespace

TypeInfo operandType(const Instr& ins, size_t idx) {
  if ((ins.op == "shl" || ins.op == "shr") && idx == 2) return parseType("u32");
  if (ins.op == "cvt") {
    auto tm = ins.typeMods();
    if (tm.size() == 2) return parseType(idx == 0 ? tm[0] : tm[1]);
  }
  return parseType(ins.typeMod());
}

bool isTerminator(const Instr& ins) {
  if (ins.raw) return false;
  return ins.op == "bra" || ins.op == "ret" || ins.op == "exit" || ins.op == "trap";
}

InstrInfo infoOf(const Instr& ins) {
  InstrInfo in;
  in.terminator = isTerminator(ins);
  if (ins.raw) {
    in.opaque = in.sideEffect = true;
    in.firstUse = -1;
    in.uses = ins.rawRegs;
    in.clobbers = ins.rawRegs;
    return in;
  }
  if (!ins.guard.empty()) in.uses.push_back(ins.guard);

  enum { Pure, EffDef, EffNoDef, Unknown } cat = Unknown;
  const std::string& op = ins.op;
  if (op == "bar") cat = ins.hasMod("red") ? EffDef : EffNoDef;
  else if (op == "ld" || op == "ldu") {
    bool strong = ins.hasMod("volatile") || ins.hasMod("acquire") || ins.hasMod("relaxed") || ins.hasMod("mmio");
    cat = strong ? EffDef : Pure;
  } else if (kPure.count(op)) {
    cat = (ins.hasMod("cc") ? EffDef : Pure);
  } else if (kEffectDef.count(op)) cat = EffDef;
  else if (kEffectNoDef.count(op)) cat = EffNoDef;

  switch (cat) {
    case Pure:
    case EffDef:
      in.removable = (cat == Pure);
      in.sideEffect = (cat == EffDef);
      in.firstUse = 1;
      if (!ins.ops.empty()) {
        const Operand& d = ins.ops[0];
        if (d.kind == OpKind::Reg || d.kind == OpKind::Pair || d.kind == OpKind::Vec)
          for (const auto& r : d.regs) in.defs.push_back(r);
      }
      for (size_t i = 1; i < ins.ops.size(); ++i) addRegs(ins.ops[i], in.uses);
      break;
    case EffNoDef:
      in.sideEffect = true;
      in.firstUse = 0;
      for (const auto& o : ins.ops) addRegs(o, in.uses);
      break;
    case Unknown:
      in.opaque = in.sideEffect = true;
      in.firstUse = -1;
      for (const auto& o : ins.ops) {
        addRegs(o, in.uses);
        addRegs(o, in.clobbers);
      }
      break;
  }
  return in;
}

bool isKnownSpecial(const std::string& name) {
  if (name.empty() || name[0] != '%') return false;
  std::string base = name.substr(1, name.find('.') == std::string::npos ? std::string::npos : name.find('.') - 1);
  static const std::set<std::string> k = {
      "tid", "ntid", "ctaid", "nctaid", "laneid", "warpid", "nwarpid", "smid", "nsmid", "gridid",
      "lanemask_eq", "lanemask_le", "lanemask_lt", "lanemask_ge", "lanemask_gt", "clock", "clock_hi",
      "clock64", "globaltimer", "globaltimer_lo", "globaltimer_hi", "cluster_ctaid", "cluster_nctaid",
      "cluster_ctarank", "cluster_nctarank", "clusterid", "nclusterid", "is_explicit_cluster",
      "total_smem_size", "aggr_smem_size", "dynamic_smem_size", "current_graph_exec"};
  if (k.count(base)) return true;
  return startsWith(base, "pm") || startsWith(base, "envreg") || startsWith(base, "reserved_smem");
}

bool isFloatLiteral(const std::string& t) {
  if (t.size() > 2 && t[0] == '0' && (t[1] == 'f' || t[1] == 'F' || t[1] == 'd' || t[1] == 'D')) return true;
  if (t.find('.') != std::string::npos) return true;
  return false;
}

bool parseIntLiteral(const std::string& text, unsigned long long& bits) {
  if (text.empty() || isFloatLiteral(text)) return false;
  std::string t = text;
  bool neg = false;
  if (t[0] == '-') { neg = true; t = t.substr(1); }
  else if (t[0] == '+') t = t.substr(1);
  while (!t.empty() && (t.back() == 'U' || t.back() == 'u' || t.back() == 'L' || t.back() == 'l')) t.pop_back();
  if (t.empty()) return false;
  try {
    size_t idx = 0;
    unsigned long long v;
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) v = std::stoull(t.substr(2), &idx, 16), idx += 2;
    else if (t.size() > 1 && t[0] == '0') v = std::stoull(t, &idx, 8);
    else v = std::stoull(t, &idx, 10);
    if (idx != t.size()) return false;
    bits = neg ? (~v + 1ULL) : v;
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace ptx
