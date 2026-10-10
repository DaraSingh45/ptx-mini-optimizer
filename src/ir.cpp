#include "ptxopt/ir.h"

#include <cctype>

#include "ptxopt/semantics.h"
#include "ptxopt/util.h"

namespace ptx {

std::string Operand::str() const {
  switch (kind) {
    case OpKind::Reg:
      return regs.empty() ? "" : regs[0];
    case OpKind::Pair:
      return regs[0] + "|" + regs[1];
    case OpKind::Vec:
      return "{" + join(regs, ", ") + "}";
    case OpKind::Mem: {
      std::string inner;
      if (!regs.empty()) inner = regs[0] + text;
      else if (!sym.empty()) inner = sym + text;
      else inner = (!text.empty() && text[0] == '+') ? text.substr(1) : text;
      return "[" + inner + "]";
    }
    default:
      return text;
  }
}

Operand makeReg(const std::string& name) {
  Operand o;
  o.kind = OpKind::Reg;
  o.regs = {name};
  return o;
}

Operand makeImm(const std::string& text) {
  Operand o;
  o.kind = OpKind::Imm;
  o.text = text;
  return o;
}

std::string Instr::full() const {
  std::string s = op;
  for (const auto& m : mods) s += "." + m;
  return s;
}

bool Instr::hasMod(const std::string& m) const {
  for (const auto& x : mods)
    if (x == m) return true;
  return false;
}

std::vector<std::string> Instr::typeMods() const {
  std::vector<std::string> r;
  for (const auto& m : mods)
    if (parseType(m).valid) r.push_back(m);
  return r;
}

std::string Instr::typeMod() const {
  for (size_t i = mods.size(); i-- > 0;)
    if (parseType(mods[i]).valid) return mods[i];
  return "";
}

std::string Instr::str() const {
  if (raw) return rawText;
  std::string s;
  if (!guard.empty()) s += std::string("@") + (guardNeg ? "!" : "") + guard + " ";
  s += full();
  if (!ops.empty()) {
    std::vector<std::string> o;
    for (const auto& x : ops) o.push_back(x.str());
    s += "\t" + join(o, ", ");
  }
  return s;
}

bool RegDecls::declared(const std::string& n) const {
  if (single.count(n)) return true;
  for (const auto& r : ranges) {
    if (n.size() <= r.prefix.size() || !startsWith(n, r.prefix)) continue;
    std::string rest = n.substr(r.prefix.size());
    bool digits = true;
    for (char c : rest)
      if (!std::isdigit(static_cast<unsigned char>(c))) digits = false;
    if (digits && rest.size() < 9 && std::stoi(rest) < r.count) return true;
  }
  return false;
}

std::string RegDecls::typeOf(const std::string& n) const {
  auto it = single.find(n);
  if (it != single.end()) return it->second;
  for (const auto& r : ranges) {
    if (n.size() <= r.prefix.size() || !startsWith(n, r.prefix)) continue;
    std::string rest = n.substr(r.prefix.size());
    bool digits = true;
    for (char c : rest)
      if (!std::isdigit(static_cast<unsigned char>(c))) digits = false;
    if (digits && rest.size() < 9 && std::stoi(rest) < r.count) return r.type;
  }
  return "";
}

int countInstrs(const Function& f) {
  int n = 0;
  for (const auto& it : f.items)
    if (!it.dead && it.kind == Item::Kind::Inst) ++n;
  return n;
}

void compact(Function& f) {
  std::vector<Item> keep;
  keep.reserve(f.items.size());
  for (auto& it : f.items)
    if (!it.dead) keep.push_back(std::move(it));
  f.items = std::move(keep);
}

std::string printFunction(const Function& f) {
  if (f.verbatim) return f.rawText + "\n";
  std::string s = f.header + "\n{\n";
  for (const auto& d : f.decls) s += "\t" + d + "\n";
  s += "\n";
  for (const auto& it : f.items) {
    if (it.dead) continue;
    switch (it.kind) {
      case Item::Kind::Label: s += it.text + ":\n"; break;
      case Item::Kind::Raw: s += "\t" + it.text + "\n"; break;
      case Item::Kind::Inst: s += "\t" + it.ins.str() + ";\n"; break;
    }
  }
  s += "}\n";
  return s;
}

std::string printModule(const Module& m) {
  std::string s = "// Processed by ptx-opt\n\n";
  for (const auto& t : m.top) {
    if (t.isFunc) s += "\n" + printFunction(m.funcs[t.func]);
    else s += t.raw + "\n";
  }
  return s;
}

}  // namespace ptx
