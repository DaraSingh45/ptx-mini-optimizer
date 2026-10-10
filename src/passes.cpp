#include "ptxopt/passes.h"

#include <algorithm>
#include <cstdint>
#include <set>

#include "ptxopt/cfg.h"
#include "ptxopt/dataflow.h"
#include "ptxopt/semantics.h"
#include "ptxopt/util.h"

namespace ptx {

namespace {

using u64 = unsigned long long;

u64 maskOf(int bits) { return bits >= 64 ? ~0ULL : ((1ULL << bits) - 1ULL); }
int64_t sext(u64 v, int bits) {
  if (bits >= 64) return static_cast<int64_t>(v);
  v &= maskOf(bits);
  u64 sign = 1ULL << (bits - 1);
  return static_cast<int64_t>((v ^ sign) - sign);
}

std::string intText(u64 v, int bits, bool isSigned) {
  v &= maskOf(bits);
  return isSigned ? std::to_string(sext(v, bits)) : std::to_string(v);
}

struct Const {
  u64 bits = 0;        // masked to `width`
  int width = 0;
  bool isFloat = false;
  bool isPred = false;
  std::string text;    // original literal (floats)
};

bool canTakeImm(const Instr& ins, size_t idx) {
  static const std::set<std::string> ops = {"add", "sub", "mul", "mad", "fma", "and", "or", "xor",
                                            "shl", "shr", "min", "max", "div", "rem", "setp"};
  if (ins.op == "mov") return idx == 1 && ins.ops.size() == 2 && ins.ops[0].kind == OpKind::Reg;
  if (ins.op == "selp") return idx == 1 || idx == 2;
  return ops.count(ins.op) && idx >= 2;
}

Instr makeMov(const std::string& type, const Operand& dst, const Operand& src, int line) {
  Instr n;
  n.op = "mov";
  n.mods = {type};
  n.ops = {dst, src};
  n.line = line;
  return n;
}

bool immInt(const Operand& o, int64_t& v) {
  if (o.kind != OpKind::Imm) return false;
  u64 b;
  if (!parseIntLiteral(o.text, b)) return false;
  v = static_cast<int64_t>(b);
  return true;
}

// ---------------------------------------------------------------- constfold
// Folds integer arithmetic on constants, propagates constants into operands,
// folds guards whose predicate is a known constant.
bool foldInt(const Instr& ins, u64& result, std::string& destType, bool& isPredResult) {
  isPredResult = false;
  TypeInfo ti = parseType(ins.typeMod());
  if (!ti.valid || ti.isFloat || ti.isPred) return false;
  const int w = ti.bits;
  const std::string& op = ins.op;
  auto src = [&](size_t i, bool& ok) -> int64_t {
    int64_t v;
    TypeInfo t = operandType(ins, i);
    if (i >= ins.ops.size() || !immInt(ins.ops[i], v) || !t.valid) { ok = false; return 0; }
    u64 m = static_cast<u64>(v) & maskOf(t.bits);
    return t.isSigned ? sext(m, t.bits) : static_cast<int64_t>(m);
  };
  bool ok = true;
  destType = ins.typeMod();
  auto done = [&](u64 r, int bits) {
    result = r & maskOf(bits);
    return ok;
  };
  const size_t n = ins.ops.size();
  if (op == "add" && ins.mods.size() == 1 && n == 3) { int64_t a = src(1, ok), b = src(2, ok); return done(static_cast<u64>(a) + static_cast<u64>(b), w); }
  if (op == "sub" && ins.mods.size() == 1 && n == 3) { int64_t a = src(1, ok), b = src(2, ok); return done(static_cast<u64>(a) - static_cast<u64>(b), w); }
  if (op == "mul" && ins.mods.size() == 2 && n == 3 && ins.mods[0] == "lo") { int64_t a = src(1, ok), b = src(2, ok); return done(static_cast<u64>(a) * static_cast<u64>(b), w); }
  if (op == "mul" && ins.mods.size() == 2 && n == 3 && ins.mods[0] == "wide" && w <= 32) {
    int64_t a = src(1, ok), b = src(2, ok);
    destType = std::string(ti.isSigned ? "s" : "u") + std::to_string(2 * w);
    return done(static_cast<u64>(a) * static_cast<u64>(b), 2 * w);
  }
  if (op == "mul" && ins.mods.size() == 2 && n == 3 && ins.mods[0] == "hi" && w <= 32) {
    int64_t a = src(1, ok), b = src(2, ok);
    u64 prod = static_cast<u64>(a) * static_cast<u64>(b);  // exact for 32x32 (signed or unsigned)
    return done(ti.isSigned ? static_cast<u64>(static_cast<int64_t>(prod) >> w) : (prod >> w), w);
  }
  if (op == "mad" && ins.mods.size() == 2 && n == 4 && ins.mods[0] == "lo") {
    int64_t a = src(1, ok), b = src(2, ok), c = src(3, ok);
    return done(static_cast<u64>(a) * static_cast<u64>(b) + static_cast<u64>(c), w);
  }
  if ((op == "and" || op == "or" || op == "xor") && ins.mods.size() == 1 && n == 3) {
    int64_t a = src(1, ok), b = src(2, ok);
    u64 r = op == "and" ? (u64)a & (u64)b : op == "or" ? ((u64)a | (u64)b) : ((u64)a ^ (u64)b);
    return done(r, w);
  }
  if (op == "not" && ins.mods.size() == 1 && n == 2) { int64_t a = src(1, ok); return done(~static_cast<u64>(a), w); }
  if (op == "neg" && ins.mods.size() == 1 && n == 2 && ti.isSigned) { int64_t a = src(1, ok); return done(0 - static_cast<u64>(a), w); }
  if ((op == "shl" || op == "shr") && ins.mods.size() == 1 && n == 3) {
    int64_t a = src(1, ok), s = src(2, ok);
    if (!ok) return false;
    u64 sh = static_cast<u64>(s);
    if (op == "shl") return done(sh >= static_cast<u64>(w) ? 0 : (static_cast<u64>(a) << sh), w);
    if (ti.isSigned) return done(static_cast<u64>(sh >= static_cast<u64>(w) ? (a < 0 ? -1 : 0) : (a >> sh)), w);
    return done(sh >= static_cast<u64>(w) ? 0 : ((static_cast<u64>(a) & maskOf(w)) >> sh), w);
  }
  if ((op == "min" || op == "max") && ins.mods.size() == 1 && n == 3) {
    int64_t a = src(1, ok), b = src(2, ok);
    if (!ok) return false;
    bool less = ti.isSigned ? a < b : static_cast<u64>(a) < static_cast<u64>(b);
    return done(static_cast<u64>((op == "min") == less ? a : b), w);
  }
  if ((op == "div" || op == "rem") && ins.mods.size() == 1 && n == 3) {
    int64_t a = src(1, ok), b = src(2, ok);
    if (!ok || b == 0) return false;
    if (ti.isSigned) {
      if (b == -1 && a == sext(1ULL << (w - 1), w)) return false;
      return done(static_cast<u64>(op == "div" ? a / b : a % b), w);
    }
    return done(op == "div" ? static_cast<u64>(a) / static_cast<u64>(b) : static_cast<u64>(a) % static_cast<u64>(b), w);
  }
  if (op == "cvt" && ins.mods.size() == 2 && n == 2) {
    auto tm = ins.typeMods();
    TypeInfo d = parseType(tm[0]), s = parseType(tm[1]);
    if (!d.valid || !s.valid || d.isFloat || s.isFloat || d.isPred || s.isPred) return false;
    int64_t a = src(1, ok);
    destType = tm[0];
    return done(static_cast<u64>(a), d.bits);
  }
  if (op == "setp" && ins.mods.size() == 2 && n == 3) {
    int64_t a = src(1, ok), b = src(2, ok);
    if (!ok) return false;
    const std::string& c = ins.mods[0];
    bool sg = ti.isSigned;
    bool lt = sg ? a < b : static_cast<u64>(a) < static_cast<u64>(b);
    bool eq = a == b;
    bool r;
    if (c == "eq") r = eq;
    else if (c == "ne") r = !eq;
    else if (c == "lt" || c == "lo") r = lt;
    else if (c == "le" || c == "ls") r = lt || eq;
    else if (c == "gt" || c == "hi") r = !lt && !eq;
    else if (c == "ge" || c == "hs") r = !lt;
    else return false;
    isPredResult = true;
    result = r ? 1 : 0;
    return true;
  }
  return false;
}

std::map<std::string, Const> globalConsts(const Function& f) {
  std::map<std::string, int> defs;
  std::map<std::string, const Instr*> movs;
  for (const auto& it : f.items) {
    if (it.dead || it.kind != Item::Kind::Inst) continue;
    InstrInfo in = infoOf(it.ins);
    for (auto& d : in.defs) defs[d] += it.ins.guard.empty() ? 1 : 2;
    for (auto& d : in.clobbers) defs[d] += 2;
    if (!it.ins.raw && it.ins.guard.empty() && it.ins.op == "mov" && it.ins.ops.size() == 2 &&
        it.ins.ops[0].kind == OpKind::Reg && it.ins.ops[1].kind == OpKind::Imm)
      movs[it.ins.ops[0].regs[0]] = &it.ins;
  }
  std::map<std::string, Const> out;
  for (auto& kv : movs) {
    if (defs[kv.first] != 1) continue;
    const Instr& m = *kv.second;
    TypeInfo t = parseType(m.typeMod());
    if (!t.valid || t.isPred || t.bits != regWidth(f.regs.typeOf(kv.first))) continue;
    Const c;
    c.width = t.bits;
    if (t.isFloat) {
      if (!isFloatLiteral(m.ops[1].text)) continue;
      c.isFloat = true;
      c.text = m.ops[1].text;
    } else {
      u64 b;
      if (!parseIntLiteral(m.ops[1].text, b)) continue;
      c.bits = b & maskOf(t.bits);
    }
    out[kv.first] = c;
  }
  return out;
}

}  // namespace

int passConstFold(Function& f) {
  int changes = 0;
  Cfg cfg = buildCfg(f);
  if (!cfg.valid) return 0;
  const auto globals = globalConsts(f);
  for (const auto& blk : cfg.blocks) {
    std::map<std::string, Const> known = globals;
    for (int i = blk.begin; i < blk.end; ++i) {
      Item& item = f.items[i];
      if (item.dead || item.kind != Item::Kind::Inst || item.ins.raw) {
        if (!item.dead && item.kind == Item::Kind::Inst) {  // opaque: forget what it may clobber
          for (auto& d : infoOf(item.ins).clobbers) known.erase(d);
        }
        continue;
      }
      Instr& ins = item.ins;
      // 1. guards with a known predicate value
      if (!ins.guard.empty()) {
        auto k = known.find(ins.guard);
        if (k != known.end() && k->second.isPred) {
          bool v = (k->second.bits & 1) != 0;
          if (ins.guardNeg ? !v : v) {
            ins.guard.clear();
            ins.guardNeg = false;
            ++changes;
          } else {
            item.dead = true;  // never executes
            ++changes;
            continue;
          }
        }
      }
      // 2. substitute constants into operands that accept immediates
      for (size_t k = 1; k < ins.ops.size(); ++k) {
        Operand& o = ins.ops[k];
        if (o.kind != OpKind::Reg || !canTakeImm(ins, k)) continue;
        auto c = known.find(o.regs[0]);
        if (c == known.end() || c->second.isPred) continue;
        TypeInfo ot = operandType(ins, k);
        if (!ot.valid || ot.isPred || ot.isFloat != c->second.isFloat) continue;
        if (ot.bits != c->second.width) continue;
        o = makeImm(c->second.isFloat ? c->second.text : intText(c->second.bits, ot.bits, ot.isSigned));
        ++changes;
      }
      // 3. fold
      Const predResult;
      bool setPred = false;
      if (ins.guard.empty() && !ins.ops.empty() && (ins.ops[0].kind == OpKind::Reg) && ins.op != "mov") {
        u64 r;
        std::string dt;
        bool isPred;
        Instr tmp = ins;  // evaluation copy: constants may appear in any source position here
        for (size_t k = 1; k < tmp.ops.size(); ++k) {
          Operand& o = tmp.ops[k];
          if (o.kind != OpKind::Reg) continue;
          auto c = known.find(o.regs[0]);
          if (c == known.end() || c->second.isPred || c->second.isFloat) continue;
          TypeInfo ot = operandType(tmp, k);
          if (!ot.valid || ot.isFloat || ot.isPred || ot.bits != c->second.width) continue;
          o = makeImm(intText(c->second.bits, ot.bits, ot.isSigned));
        }
        if (foldInt(tmp, r, dt, isPred)) {
          if (isPred) {
            predResult.isPred = true;
            predResult.bits = r;
            predResult.width = 1;
            setPred = true;
          } else {
            TypeInfo d = parseType(dt);
            int declared = regWidth(f.regs.typeOf(ins.ops[0].regs[0]));
            if (d.valid && (declared == 0 || declared == d.bits)) {
              ins = makeMov(dt, ins.ops[0], makeImm(intText(r, d.bits, d.isSigned)), ins.line);
              ++changes;
            }
          }
        }
      }
      // 4. update knowledge
      InstrInfo in = infoOf(ins);
      for (auto& d : in.defs) known.erase(d);
      for (auto& d : in.clobbers) known.erase(d);
      if (setPred && ins.ops[0].kind == OpKind::Reg) known[ins.ops[0].regs[0]] = predResult;
      if (ins.guard.empty() && ins.op == "mov" && ins.ops.size() == 2 && ins.ops[0].kind == OpKind::Reg &&
          ins.ops[1].kind == OpKind::Imm) {
        TypeInfo t = parseType(ins.typeMod());
        if (t.valid && !t.isPred && t.bits == regWidth(f.regs.typeOf(ins.ops[0].regs[0]))) {
          Const c;
          c.width = t.bits;
          u64 b;
          if (t.isFloat && isFloatLiteral(ins.ops[1].text)) {
            c.isFloat = true;
            c.text = ins.ops[1].text;
            known[ins.ops[0].regs[0]] = c;
          } else if (!t.isFloat && parseIntLiteral(ins.ops[1].text, b)) {
            c.bits = b & maskOf(t.bits);
            known[ins.ops[0].regs[0]] = c;
          }
        }
      }
    }
  }
  compact(f);
  return changes;
}

// ---------------------------------------------------------------- copyprop
int passCopyProp(Function& f) {
  int changes = 0;
  Cfg cfg = buildCfg(f);
  if (!cfg.valid) return 0;

  // Global part: `mov d, s` where both d and s have exactly one definition in the
  // whole function means d == s everywhere d is defined (nvcc output is nearly SSA).
  {
    std::map<std::string, int> defCount;
    std::map<std::string, std::string> cand;
    for (const auto& it : f.items) {
      if (it.dead || it.kind != Item::Kind::Inst) continue;
      InstrInfo in = infoOf(it.ins);
      for (auto& d : in.defs) defCount[d] += it.ins.guard.empty() ? 1 : 2;
      for (auto& d : in.clobbers) defCount[d] += 2;
      const Instr& m = it.ins;
      if (!m.raw && m.guard.empty() && m.op == "mov" && m.ops.size() == 2 && m.ops[0].kind == OpKind::Reg &&
          m.ops[1].kind == OpKind::Reg)
        cand[m.ops[0].regs[0]] = m.ops[1].regs[0];
    }
    std::map<std::string, std::string> g;
    for (auto& kv : cand) {
      const std::string &d = kv.first, &s = kv.second;
      std::string td = f.regs.typeOf(d), ts = f.regs.typeOf(s);
      if (d != s && defCount[d] == 1 && defCount[s] == 1 && !td.empty() && td == ts) g[d] = s;
    }
    auto root = [&](std::string r) {
      for (int hops = 0; hops < 32; ++hops) {
        auto k = g.find(r);
        if (k == g.end()) break;
        r = k->second;
      }
      return r;
    };
    if (!g.empty()) {
      for (auto& it : f.items) {
        if (it.dead || it.kind != Item::Kind::Inst) continue;
        InstrInfo in = infoOf(it.ins);
        if (in.opaque) continue;
        auto rep = [&](std::string& r) {
          std::string n = root(r);
          if (n != r) { r = n; ++changes; }
        };
        if (!it.ins.guard.empty()) rep(it.ins.guard);
        for (size_t k = static_cast<size_t>(in.firstUse); k < it.ins.ops.size(); ++k) {
          Operand& o = it.ins.ops[k];
          if (o.kind == OpKind::Reg || o.kind == OpKind::Vec || o.kind == OpKind::Mem)
            for (auto& r : o.regs) rep(r);
        }
      }
    }
  }

  for (const auto& blk : cfg.blocks) {
    std::map<std::string, std::string> copy;  // dst -> src
    for (int i = blk.begin; i < blk.end; ++i) {
      Item& item = f.items[i];
      if (item.dead || item.kind != Item::Kind::Inst) continue;
      Instr& ins = item.ins;
      InstrInfo info = infoOf(ins);
      if (!info.opaque) {
        auto rep = [&](std::string& r) {
          auto k = copy.find(r);
          if (k != copy.end()) {
            r = k->second;
            ++changes;
          }
        };
        if (!ins.guard.empty()) rep(ins.guard);
        for (size_t k = static_cast<size_t>(info.firstUse); k < ins.ops.size(); ++k) {
          Operand& o = ins.ops[k];
          if (o.kind == OpKind::Reg || o.kind == OpKind::Vec || o.kind == OpKind::Mem)
            for (auto& r : o.regs) rep(r);
        }
        info = infoOf(ins);
      }
      for (auto& d : info.defs) {
        copy.erase(d);
        for (auto it = copy.begin(); it != copy.end();) it = (it->second == d) ? copy.erase(it) : std::next(it);
      }
      for (auto& d : info.clobbers) {
        copy.erase(d);
        for (auto it = copy.begin(); it != copy.end();) it = (it->second == d) ? copy.erase(it) : std::next(it);
      }
      if (!ins.raw && ins.guard.empty() && ins.op == "mov" && ins.ops.size() == 2 && ins.ops[0].kind == OpKind::Reg &&
          ins.ops[1].kind == OpKind::Reg) {
        const std::string &d = ins.ops[0].regs[0], &s = ins.ops[1].regs[0];
        std::string td = f.regs.typeOf(d), ts = f.regs.typeOf(s);
        if (d != s && !td.empty() && td == ts) copy[d] = s;
      }
    }
  }
  return changes;
}

// --------------------------------------------------------------------- cse
int passCse(Function& f) {
  int changes = 0;
  Cfg cfg = buildCfg(f);
  if (!cfg.valid) return 0;
  struct Entry {
    std::string dest;
    std::set<std::string> deps;
  };
  static const std::set<std::string> commutative = {"add", "mul", "and", "or", "xor", "min", "max"};
  for (const auto& blk : cfg.blocks) {
    std::map<std::string, Entry> table;
    for (int i = blk.begin; i < blk.end; ++i) {
      Item& item = f.items[i];
      if (item.dead || item.kind != Item::Kind::Inst) continue;
      Instr& ins = item.ins;
      InstrInfo info = infoOf(ins);
      bool cseable = !ins.raw && ins.guard.empty() && info.removable && info.defs.size() == 1 &&
                     !ins.ops.empty() && ins.ops[0].kind == OpKind::Reg &&
                     (ins.op != "mov" || (ins.ops.size() == 2 && ins.ops[1].kind == OpKind::Special)) &&
                     (ins.op != "ld" || ins.hasMod("param"));
      std::string key;
      if (cseable) {
        std::vector<std::string> srcs;
        for (size_t k = 1; k < ins.ops.size(); ++k) srcs.push_back(ins.ops[k].str());
        if (commutative.count(ins.op) && srcs.size() == 2 && srcs[0] > srcs[1]) std::swap(srcs[0], srcs[1]);
        key = ins.full() + " " + join(srcs, ",");
      }
      bool replaced = false;
      if (cseable) {
        auto k = table.find(key);
        std::string ty = f.regs.typeOf(ins.ops[0].regs[0]);
        if (k != table.end() && !ty.empty() && f.regs.typeOf(k->second.dest) == ty) {
          ins = makeMov(ty, ins.ops[0], makeReg(k->second.dest), ins.line);
          replaced = true;
          ++changes;
        }
      }
      for (auto& d : info.defs) {
        for (auto it = table.begin(); it != table.end();)
          it = (it->second.dest == d || it->second.deps.count(d)) ? table.erase(it) : std::next(it);
      }
      for (auto& d : info.clobbers) {
        for (auto it = table.begin(); it != table.end();)
          it = (it->second.dest == d || it->second.deps.count(d)) ? table.erase(it) : std::next(it);
      }
      if (cseable && !replaced) {
        const std::string& d = info.defs[0];
        if (std::find(info.uses.begin(), info.uses.end(), d) == info.uses.end()) {
          Entry e;
          e.dest = d;
          e.deps.insert(info.uses.begin(), info.uses.end());
          table[key] = e;
        }
      }
    }
  }
  return changes;
}

// ---------------------------------------------------------------- simplify
int passSimplify(Function& f) {
  int changes = 0;
  for (auto& item : f.items) {
    if (item.dead || item.kind != Item::Kind::Inst || item.ins.raw) continue;
    Instr& ins = item.ins;
    const std::string T = ins.typeMod();
    TypeInfo ti = parseType(T);
    const std::string op = ins.op;
    if (op == "mov" && ins.ops.size() == 2 && ins.ops[0].kind == OpKind::Reg && ins.ops[1].kind == OpKind::Reg &&
        ins.ops[0].regs[0] == ins.ops[1].regs[0]) {
      item.dead = true;
      ++changes;
      continue;
    }
    if (op == "cvt" && ins.mods.size() == 2 && ins.mods[0] == ins.mods[1] && ti.valid && !ti.isFloat && !ti.isPred &&
        ins.ops.size() == 2) {
      ins.op = "mov";
      ins.mods = {T};
      ++changes;
      continue;
    }
    if (!ti.valid || ti.isFloat || ti.isPred || ins.ops.size() < 3 || ins.ops[0].kind != OpKind::Reg) continue;
    const int w = ti.bits;
    int64_t va = 0, vb = 0;
    bool ia = immInt(ins.ops[1], va), ib = immInt(ins.ops[2], vb);
    auto toMov = [&](size_t src) {
      Operand s = ins.ops[src], d = ins.ops[0];
      ins = makeMov(T, d, s, ins.line);
      ++changes;
    };
    auto toZero = [&]() {
      Operand d = ins.ops[0];
      ins = makeMov(T, d, makeImm("0"), ins.line);
      ++changes;
    };
    auto isPow2 = [&](int64_t v, int& k) {
      if (v <= 1 || (v & (v - 1)) != 0) return false;
      k = 0;
      while ((1LL << k) != v) ++k;
      return k < w;
    };
    const bool oneMod = ins.mods.size() == 1;
    if ((op == "add" || op == "or" || op == "xor") && oneMod && ins.ops.size() == 3) {
      if (ib && vb == 0) toMov(1);
      else if (ia && va == 0) toMov(2);
    } else if ((op == "sub" || op == "shl" || op == "shr") && oneMod && ins.ops.size() == 3) {
      if (ib && vb == 0) toMov(1);
    } else if (op == "and" && oneMod && ins.ops.size() == 3) {
      u64 all = maskOf(w);
      if ((ib && (static_cast<u64>(vb) & all) == all) ) toMov(1);
      else if (ia && (static_cast<u64>(va) & all) == all) toMov(2);
      else if ((ib && vb == 0) || (ia && va == 0)) toZero();
    } else if (op == "mul" && ins.mods.size() == 2 && ins.mods[0] == "lo" && ins.ops.size() == 3) {
      int k;
      if (ib && vb == 1) toMov(1);
      else if (ia && va == 1) toMov(2);
      else if ((ib && vb == 0) || (ia && va == 0)) toZero();
      else if (ib && isPow2(vb, k)) {
        Operand d = ins.ops[0], a = ins.ops[1];
        ins.op = "shl";
        ins.mods = {"b" + std::to_string(w)};
        ins.ops = {d, a, makeImm(std::to_string(k))};
        ++changes;
      } else if (ia && isPow2(va, k)) {
        Operand d = ins.ops[0], b = ins.ops[2];
        ins.op = "shl";
        ins.mods = {"b" + std::to_string(w)};
        ins.ops = {d, b, makeImm(std::to_string(k))};
        ++changes;
      }
    } else if ((op == "div" || op == "rem") && oneMod && ins.ops.size() == 3 && ib) {
      int k;
      if (op == "div" && vb == 1) toMov(1);
      else if (!ti.isSigned && ins.mods[0][0] == 'u' && isPow2(vb, k)) {
        Operand d = ins.ops[0], a = ins.ops[1];
        if (op == "div") {
          ins.op = "shr";
          ins.mods = {"u" + std::to_string(w)};
          ins.ops = {d, a, makeImm(std::to_string(k))};
        } else {
          ins.op = "and";
          ins.mods = {"b" + std::to_string(w)};
          ins.ops = {d, a, makeImm(std::to_string(vb - 1))};
        }
        ++changes;
      }
    }
  }
  compact(f);
  return changes;
}

// ------------------------------------------------------------------ branch
int passBranch(Function& f) {
  int changes = 0;
  for (int round = 0; round < 8; ++round) {
    bool again = false;
    Cfg cfg = buildCfg(f);
    if (!cfg.valid) return changes;

    // (a) jump threading through blocks that only contain `bra X`
    std::map<std::string, std::string> fwd;
    for (const auto& b : cfg.blocks) {
      int li = cfg.lastInstr(f, static_cast<int>(&b - &cfg.blocks[0]));
      if (li < 0) continue;
      const Instr& t = f.items[li].ins;
      if (t.op != "bra" || !t.guard.empty() || t.raw) continue;
      bool only = true;
      for (int i = b.begin; i < b.end; ++i)
        if (!f.items[i].dead && f.items[i].kind == Item::Kind::Raw) only = false;
      int n = 0;
      for (int i = b.begin; i < b.end; ++i)
        if (!f.items[i].dead && f.items[i].kind == Item::Kind::Inst) ++n;
      if (!only || n != 1) continue;
      for (const auto& l : b.labels) fwd[l] = t.ops[0].text;
    }
    auto resolve = [&](std::string l) {
      for (int hops = 0; hops < 16; ++hops) {
        auto k = fwd.find(l);
        if (k == fwd.end() || k->second == l) break;
        l = k->second;
      }
      return l;
    };
    for (auto& it : f.items) {
      if (it.dead || it.kind != Item::Kind::Inst || it.ins.raw || it.ins.op != "bra" || it.ins.ops.empty()) continue;
      std::string r = resolve(it.ins.ops[0].text);
      if (r != it.ins.ops[0].text) {
        it.ins.ops[0].text = r;
        ++changes;
        again = true;
      }
    }

    // (b) branch to the immediately following label is a no-op
    for (size_t i = 0; i < f.items.size(); ++i) {
      Item& it = f.items[i];
      if (it.dead || it.kind != Item::Kind::Inst || it.ins.raw || it.ins.op != "bra" || it.ins.ops.empty()) continue;
      const std::string target = it.ins.ops[0].text;
      for (size_t j = i + 1; j < f.items.size(); ++j) {
        const Item& nx = f.items[j];
        if (nx.dead) continue;
        if (nx.kind == Item::Kind::Label) {
          if (nx.text == target) {
            it.dead = true;
            ++changes;
            again = true;
            break;
          }
          continue;
        }
        if (nx.kind == Item::Kind::Raw) continue;
        break;
      }
    }
    compact(f);

    // (c) unreachable blocks
    cfg = buildCfg(f);
    if (!cfg.valid) return changes;
    for (size_t b = 0; b < cfg.blocks.size(); ++b) {
      if (cfg.reachable[b]) continue;
      for (int i = cfg.blocks[b].begin; i < cfg.blocks[b].end; ++i) {
        if (!f.items[i].dead) {
          f.items[i].dead = true;
          if (f.items[i].kind == Item::Kind::Inst) ++changes;
          again = true;
        }
      }
    }
    compact(f);

    // (d) labels nobody jumps to
    std::set<std::string> referenced;
    for (const auto& it : f.items)
      if (!it.dead && it.kind == Item::Kind::Inst && it.ins.op == "bra" && !it.ins.ops.empty())
        referenced.insert(it.ins.ops[0].text);
    for (auto& it : f.items)
      if (it.kind == Item::Kind::Label && !referenced.count(it.text)) {
        it.dead = true;
        ++changes;
        again = true;
      }
    compact(f);
    if (!again) break;
  }
  return changes;
}

// --------------------------------------------------------------------- dce
int passDce(Function& f) {
  int total = 0;
  for (int round = 0; round < 50; ++round) {
    Cfg cfg = buildCfg(f);
    if (!cfg.valid) break;
    Liveness lv = computeLiveness(f, cfg);
    int removed = 0;
    for (size_t b = 0; b < cfg.blocks.size(); ++b) {
      BitSet live = lv.out[b];
      for (int i = cfg.blocks[b].end - 1; i >= cfg.blocks[b].begin; --i) {
        Item& it = f.items[i];
        if (it.dead || it.kind != Item::Kind::Inst) continue;
        InstrInfo info = infoOf(it.ins);
        if (info.removable && !info.defs.empty()) {
          bool anyLive = false;
          for (auto& d : info.defs)
            if (live.test(lv.idOf(d))) anyLive = true;
          if (!anyLive) {
            it.dead = true;
            ++removed;
            continue;
          }
        }
        livenessTransfer(lv, it.ins, live);
      }
    }
    compact(f);
    total += removed;
    if (!removed) break;
  }
  return total;
}

// ---------------------------------------------------------------- pipeline
const std::vector<PassInfo>& allPasses() {
  static const std::vector<PassInfo> p = {
      {"constfold", "constant propagation/folding, constant guards", passConstFold},
      {"copyprop", "block-local + single-definition copy propagation", passCopyProp},
      {"cse", "block-local common subexpression elimination", passCse},
      {"simplify", "peephole: identities, strength reduction, self-moves", passSimplify},
      {"branch", "jump threading, redundant/unreachable branch+block removal", passBranch},
      {"dce", "liveness-based dead code elimination", passDce},
  };
  return p;
}

std::vector<std::string> defaultPipeline() {
  std::vector<std::string> r;
  for (const auto& p : allPasses()) r.push_back(p.name);
  return r;
}

OptStats optimizeFunction(Function& f, const std::vector<std::string>& passes, int maxIter) {
  OptStats st;
  if (f.verbatim) return st;
  for (int it = 0; it < maxIter; ++it) {
    int round = 0;
    for (const auto& name : passes) {
      for (const auto& p : allPasses()) {
        if (name != p.name) continue;
        int c = p.run(f);
        st.perPass[name] += c;
        round += c;
      }
    }
    ++st.iterations;
    if (round == 0) break;
  }
  return st;
}

}  // namespace ptx
