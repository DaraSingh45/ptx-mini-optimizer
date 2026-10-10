#include "ptxopt/interp.h"

#include <cmath>
#include <cstring>
#include <random>
#include <unordered_map>

#include "ptxopt/lexer.h"
#include "ptxopt/semantics.h"
#include "ptxopt/util.h"

namespace ptx {

namespace {
using u64 = unsigned long long;
u64 mk(int b) { return b >= 64 ? ~0ULL : ((1ULL << b) - 1ULL); }
int64_t sx(u64 v, int b) {
  if (b >= 64) return static_cast<int64_t>(v);
  v &= mk(b);
  u64 s = 1ULL << (b - 1);
  return static_cast<int64_t>((v ^ s) - s);
}
float asF(u64 b) { uint32_t x = static_cast<uint32_t>(b); float f; std::memcpy(&f, &x, 4); return f; }
double asD(u64 b) { double d; std::memcpy(&d, &b, 8); return d; }
u64 fromF(float f) { uint32_t x; std::memcpy(&x, &f, 4); return x; }
u64 fromD(double d) { u64 x; std::memcpy(&x, &d, 8); return x; }
double toD(u64 b, int w) {
  if (w == 32) return asF(b);
  if (w == 64) return asD(b);
  throw Unsupported("16-bit floats");
}
u64 fromDouble(double d, int w) { return w == 32 ? fromF(static_cast<float>(d)) : fromD(d); }
}  // namespace

uint64_t Memory::read(uint64_t a, int n) const {
  uint64_t v = 0;
  for (int i = 0; i < n; ++i) {
    auto it = bytes.find(a + i);
    if (it != bytes.end()) v |= static_cast<uint64_t>(it->second) << (8 * i);
  }
  return v;
}
void Memory::write(uint64_t a, int n, uint64_t v) {
  for (int i = 0; i < n; ++i) bytes[a + i] = static_cast<uint8_t>(v >> (8 * i));
}

namespace {

class Machine {
 public:
  Machine(const Function& f, ExecEnv& e) : f_(f), env_(e) {
    int idx = 0;
    for (const auto& it : f.items) {
      if (it.kind != Item::Kind::Raw) continue;
      bool local = startsWith(it.text, ".local"), shared = startsWith(it.text, ".shared");
      if (!local && !shared) continue;
      size_t br = it.text.find('[');
      if (br == std::string::npos) continue;
      size_t b = br;
      while (b > 0 && (std::isalnum(static_cast<unsigned char>(it.text[b - 1])) || it.text[b - 1] == '_' || it.text[b - 1] == '$')) --b;
      std::string name = it.text.substr(b, br - b);
      u64 base = local ? (0x70000000ULL + idx * 0x4000000ULL + e.threadLinear * 0x10000ULL) : (0x60000000ULL + idx * 0x100000ULL);
      syms_[name] = base;
      ++idx;
    }
  }

  ExecResult run(long maxSteps) {
    ExecResult r;
    for (size_t i = 0; i < f_.items.size(); ++i)
      if (f_.items[i].kind == Item::Kind::Label) labels_[f_.items[i].text] = i;
    size_t pc = 0;
    try {
      while (pc < f_.items.size()) {
        if (++r.steps > maxSteps) {
          r.ok = false;
          r.error = "step limit exceeded";
          return r;
        }
        const Item& it = f_.items[pc];
        if (it.kind != Item::Kind::Inst) { ++pc; continue; }
        const Instr& ins = it.ins;
        if (ins.raw) throw Unsupported("opaque instruction: " + ins.rawText);
        if (!ins.guard.empty()) {
          bool g = (reg(ins.guard) & 1) != 0;
          if (ins.guardNeg) g = !g;
          if (!g) { ++pc; continue; }
        }
        if (ins.op == "ret" || ins.op == "exit") return r;
        if (ins.op == "bra") {
          auto k = labels_.find(ins.ops.at(0).text);
          if (k == labels_.end()) throw Unsupported("branch to unknown label");
          pc = k->second;
          continue;
        }
        exec(ins);
        ++pc;
      }
    } catch (const Unsupported& e) {
      r.ok = false;
      r.unsupported = true;
      r.error = e.what();
    }
    return r;
  }

 private:
  const Function& f_;
  ExecEnv& env_;
  std::unordered_map<std::string, u64> regs_;
  std::map<std::string, size_t> labels_;
  std::map<std::string, u64> syms_;

  u64 reg(const std::string& n) const {
    auto it = regs_.find(n);
    return it == regs_.end() ? 0 : it->second;
  }
  void writeReg(const std::string& n, u64 v) {
    int w = regWidth(f_.regs.typeOf(n));
    if (f_.regs.typeOf(n) == "pred") w = 1;
    regs_[n] = w ? (v & mk(w)) : v;
  }
  void writeDst(const Operand& o, u64 v) {
    if (o.kind != OpKind::Reg) throw Unsupported("destination operand kind");
    writeReg(o.regs[0], v);
  }
  u64 special(const std::string& n) const {
    auto idx = [&](const std::string& s) { return s.back() == 'x' ? 0 : s.back() == 'y' ? 1 : 2; };
    if (startsWith(n, "%tid.")) return env_.tid[idx(n)];
    if (startsWith(n, "%ntid.")) return env_.ntid[idx(n)];
    if (startsWith(n, "%ctaid.")) return env_.ctaid[idx(n)];
    if (startsWith(n, "%nctaid.")) return env_.nctaid[idx(n)];
    if (n == "%laneid") return env_.tid[0] % 32;
    if (n == "%warpid") return env_.tid[0] / 32;
    throw Unsupported("special register " + n);
  }
  u64 val(const Operand& o, const TypeInfo& t) {
    int bits = t.bits ? t.bits : 64;
    switch (o.kind) {
      case OpKind::Reg: return reg(o.regs[0]) & mk(bits);
      case OpKind::Special: return special(o.text) & mk(bits);
      case OpKind::Sym: {
        auto it = syms_.find(o.text);
        if (it == syms_.end()) throw Unsupported("unknown symbol " + o.text);
        return it->second & mk(bits);
      }
      case OpKind::Imm: {
        if (isFloatLiteral(o.text)) {
          if (!t.isFloat) throw Unsupported("float literal in integer context");
          if (o.text.size() > 2 && (o.text[1] == 'f' || o.text[1] == 'F')) return std::stoull(o.text.substr(2), nullptr, 16) & mk(32);
          if (o.text.size() > 2 && (o.text[1] == 'd' || o.text[1] == 'D')) return std::stoull(o.text.substr(2), nullptr, 16);
          return fromDouble(std::stod(o.text), bits);
        }
        u64 b;
        if (!parseIntLiteral(o.text, b)) throw Unsupported("bad literal " + o.text);
        if (t.isFloat) throw Unsupported("integer literal in float context");
        return b & mk(bits);
      }
      default: throw Unsupported("operand kind");
    }
  }
  u64 address(const Operand& o) {
    if (o.kind != OpKind::Mem) throw Unsupported("expected memory operand");
    u64 base = 0;
    if (!o.regs.empty()) base = reg(o.regs[0]);
    else if (!o.sym.empty()) {
      auto it = syms_.find(o.sym);
      if (it == syms_.end()) throw Unsupported("unknown symbol " + o.sym);
      base = it->second;
    }
    u64 off = 0;
    if (!o.text.empty() && !parseIntLiteral(o.text, off)) throw Unsupported("bad offset");
    return base + off;
  }

  static bool cmpInt(const std::string& c, int64_t a, int64_t b, bool sg) {
    bool lt = sg ? a < b : static_cast<u64>(a) < static_cast<u64>(b), eq = a == b;
    if (c == "eq") return eq;
    if (c == "ne") return !eq;
    if (c == "lt" || c == "lo") return lt;
    if (c == "le" || c == "ls") return lt || eq;
    if (c == "gt" || c == "hi") return !lt && !eq;
    if (c == "ge" || c == "hs") return !lt;
    throw Unsupported("comparison " + c);
  }
  static bool cmpFloat(std::string c, double a, double b) {
    bool nan = std::isnan(a) || std::isnan(b);
    if (c == "num") return !nan;
    if (c == "nan") return nan;
    bool unordered = c.size() == 3 && c.back() == 'u';
    if (unordered) c.pop_back();
    if (nan) return unordered;
    if (c == "eq") return a == b;
    if (c == "ne") return a != b;
    if (c == "lt") return a < b;
    if (c == "le") return a <= b;
    if (c == "gt") return a > b;
    if (c == "ge") return a >= b;
    throw Unsupported("comparison " + c);
  }

  void exec(const Instr& ins) {
    const std::string& op = ins.op;
    const TypeInfo t = parseType(ins.typeMod());
    const auto& o = ins.ops;
    auto I = [&](size_t i) -> int64_t {
      TypeInfo ot = operandType(ins, i);
      u64 m = val(o.at(i), ot);
      return ot.isSigned ? sx(m, ot.bits) : static_cast<int64_t>(m);
    };
    auto F = [&](size_t i) { return toD(val(o.at(i), t), t.bits); };
    auto needType = [&]() {
      if (!t.valid) throw Unsupported("instruction without type: " + ins.full());
    };

    if (op == "membar" || op == "fence") return;
    if (op == "mov") {
      if (o.size() != 2 || o[0].kind != OpKind::Reg) throw Unsupported("mov form");
      needType();
      writeDst(o[0], val(o[1], t));
      return;
    }
    if (op == "cvta") { writeDst(o.at(0), val(o.at(1), parseType("u64"))); return; }
    if (op == "ld" || op == "st") { execMem(ins, t); return; }
    needType();
    const int w = t.bits;
    if (t.isFloat) {
      if (w != 32 && w != 64) throw Unsupported("16-bit float op");
      double r;
      if (op == "add") r = F(1) + F(2);
      else if (op == "sub") r = F(1) - F(2);
      else if (op == "mul") r = F(1) * F(2);
      else if (op == "div") r = F(1) / F(2);
      else if (op == "fma" || op == "mad") {
        if (w == 32) { writeDst(o[0], fromF(std::fma(static_cast<float>(F(1)), static_cast<float>(F(2)), static_cast<float>(F(3))))); return; }
        r = std::fma(F(1), F(2), F(3));
      }
      else if (op == "neg") r = -F(1);
      else if (op == "abs") r = std::fabs(F(1));
      else if (op == "min") r = std::fmin(F(1), F(2));
      else if (op == "max") r = std::fmax(F(1), F(2));
      else if (op == "rcp") r = 1.0 / F(1);
      else if (op == "sqrt") r = std::sqrt(F(1));
      else if (op == "rsqrt") r = 1.0 / std::sqrt(F(1));
      else if (op == "ex2") r = std::exp2(F(1));
      else if (op == "lg2") r = std::log2(F(1));
      else if (op == "sin") r = std::sin(F(1));
      else if (op == "cos") r = std::cos(F(1));
      else if (op == "setp") {
        if (ins.mods.size() != 2) throw Unsupported("setp form");
        bool b = cmpFloat(ins.mods[0], F(1), F(2));
        writeSetp(o[0], b);
        return;
      }
      else if (op == "selp") { bool p = (val(o.at(3), parseType("pred")) & 1) != 0; writeDst(o[0], val(p ? o.at(1) : o.at(2), t)); return; }
      else if (op == "cvt") { execCvt(ins); return; }
      else throw Unsupported("float op " + ins.full());
      writeDst(o[0], fromDouble(r, w));
      return;
    }
    if (op == "cvt") { execCvt(ins); return; }
    if (t.isPred) {
      if (op == "and" || op == "or" || op == "xor" || op == "not") {
        u64 a = val(o.at(1), t) & 1, b = o.size() > 2 ? (val(o[2], t) & 1) : 0;
        writeDst(o[0], op == "and" ? (a & b) : op == "or" ? (a | b) : op == "xor" ? (a ^ b) : (a ^ 1));
        return;
      }
      if (op == "selp") { bool p = (val(o.at(3), t) & 1) != 0; writeDst(o[0], val(p ? o.at(1) : o.at(2), t)); return; }
      throw Unsupported("predicate op " + ins.full());
    }
    const bool sg = t.isSigned;
    u64 r;
    if (op == "add") r = static_cast<u64>(I(1)) + static_cast<u64>(I(2));
    else if (op == "sub") r = static_cast<u64>(I(1)) - static_cast<u64>(I(2));
    else if (op == "mul" || op == "mad") {
      if (ins.mods.empty()) throw Unsupported("mul without lo/hi/wide");
      const std::string& m = ins.mods[0];
      int64_t a = I(1), b = I(2);
      u64 prod = static_cast<u64>(a) * static_cast<u64>(b);
      if (m == "lo") r = prod;
      else if (m == "wide") {
        if (w > 32) throw Unsupported("mul.wide 64");
        r = prod;
      } else if (m == "hi") {
        if (w > 32) throw Unsupported("mul.hi 64");
        r = sg ? static_cast<u64>(static_cast<int64_t>(prod) >> w) : (prod >> w);
      } else throw Unsupported("mul variant " + m);
      if (op == "mad") {
        u64 c;
        if (m == "wide") {
          TypeInfo ct = parseType(std::string(sg ? "s" : "u") + std::to_string(2 * w));
          u64 cv = val(o.at(3), ct);
          c = sg ? static_cast<u64>(sx(cv, ct.bits)) : cv;
        } else {
          c = static_cast<u64>(I(3));
        }
        r += c;
      }
    }
    else if (op == "div" || op == "rem") {
      int64_t a = I(1), b = I(2);
      if (b == 0) throw Unsupported("division by zero");
      if (sg) r = static_cast<u64>(op == "div" ? a / b : a % b);
      else r = op == "div" ? static_cast<u64>(a) / static_cast<u64>(b) : static_cast<u64>(a) % static_cast<u64>(b);
    }
    else if (op == "and") r = static_cast<u64>(I(1)) & static_cast<u64>(I(2));
    else if (op == "or") r = static_cast<u64>(I(1)) | static_cast<u64>(I(2));
    else if (op == "xor") r = static_cast<u64>(I(1)) ^ static_cast<u64>(I(2));
    else if (op == "not") r = ~static_cast<u64>(I(1));
    else if (op == "neg") r = 0 - static_cast<u64>(I(1));
    else if (op == "abs") { int64_t a = I(1); r = static_cast<u64>(a < 0 ? -a : a); }
    else if (op == "shl" || op == "shr") {
      int64_t a = I(1);
      u64 s = static_cast<u64>(I(2));
      if (op == "shl") r = s >= static_cast<u64>(w) ? 0 : (static_cast<u64>(a) << s);
      else if (sg) r = static_cast<u64>(s >= static_cast<u64>(w) ? (a < 0 ? -1 : 0) : (a >> s));
      else r = s >= static_cast<u64>(w) ? 0 : ((static_cast<u64>(a) & mk(w)) >> s);
    }
    else if (op == "min" || op == "max") {
      int64_t a = I(1), b = I(2);
      bool less = sg ? a < b : static_cast<u64>(a) < static_cast<u64>(b);
      r = static_cast<u64>((op == "min") == less ? a : b);
    }
    else if (op == "selp") { bool p = (val(o.at(3), parseType("pred")) & 1) != 0; r = val(p ? o.at(1) : o.at(2), t); }
    else if (op == "setp") {
      if (ins.mods.size() != 2) throw Unsupported("setp form");
      writeSetp(o[0], cmpInt(ins.mods[0], I(1), I(2), sg));
      return;
    }
    else throw Unsupported("integer op " + ins.full());
    writeDst(o[0], r & mk(w == 0 ? 64 : (op == "mul" || op == "mad") && ins.mods[0] == "wide" ? 2 * w : w));
  }

  void writeSetp(const Operand& d, bool b) {
    if (d.kind == OpKind::Reg) writeReg(d.regs[0], b ? 1 : 0);
    else if (d.kind == OpKind::Pair) { writeReg(d.regs[0], b ? 1 : 0); writeReg(d.regs[1], b ? 0 : 1); }
    else throw Unsupported("setp destination");
  }

  void execCvt(const Instr& ins) {
    auto tm = ins.typeMods();
    if (tm.size() != 2) throw Unsupported("cvt form");
    TypeInfo d = parseType(tm[0]), s = parseType(tm[1]);
    const auto& o = ins.ops;
    std::string rnd;
    for (const char* m : {"rni", "rzi", "rmi", "rpi"})
      if (ins.hasMod(m)) rnd = m;
    bool sat = ins.hasMod("sat");
    u64 raw = val(o.at(1), s);
    double x = 0;
    int64_t iv = 0;
    if (s.isFloat) x = toD(raw, s.bits);
    else iv = s.isSigned ? sx(raw, s.bits) : static_cast<int64_t>(raw);
    if (d.isFloat) {
      double r = s.isFloat ? x : (s.isSigned ? static_cast<double>(iv) : static_cast<double>(static_cast<u64>(iv)));
      writeDst(o[0], fromDouble(r, d.bits));
      return;
    }
    u64 res;
    if (s.isFloat) {
      double r = std::isnan(x) ? 0.0 : (rnd == "rni" ? std::nearbyint(x) : rnd == "rmi" ? std::floor(x) : rnd == "rpi" ? std::ceil(x) : std::trunc(x));
      double lo = d.isSigned ? -std::ldexp(1.0, d.bits - 1) : 0.0, hi = d.isSigned ? std::ldexp(1.0, d.bits - 1) - 1 : std::ldexp(1.0, d.bits) - 1;
      if (r < lo) r = lo;
      if (r > hi) r = hi;
      res = d.isSigned ? static_cast<u64>(static_cast<int64_t>(r)) : static_cast<u64>(r);
    } else {
      res = static_cast<u64>(iv);
      if (sat) {
        __int128 v = s.isSigned ? static_cast<__int128>(iv) : static_cast<__int128>(static_cast<u64>(iv));
        __int128 lo = d.isSigned ? -(static_cast<__int128>(1) << (d.bits - 1)) : 0;
        __int128 hi = d.isSigned ? (static_cast<__int128>(1) << (d.bits - 1)) - 1 : (static_cast<__int128>(1) << d.bits) - 1;
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        res = static_cast<u64>(v);
      }
    }
    res = d.isSigned ? static_cast<u64>(sx(res, d.bits)) : (res & mk(d.bits));
    writeDst(o[0], res);
  }

  void execMem(const Instr& ins, const TypeInfo& t) {
    if (!t.valid || t.isPred) throw Unsupported("memory op type");
    const auto& o = ins.ops;
    int n = ins.hasMod("v2") ? 2 : ins.hasMod("v4") ? 4 : 1;
    int bytes = t.bits / 8;
    if (ins.op == "ld") {
      if (ins.hasMod("param")) {
        if (o.at(1).kind != OpKind::Mem || !o[1].text.empty()) throw Unsupported("ld.param with offset");
        auto it = env_.params.find(o[1].sym);
        if (it == env_.params.end()) throw Unsupported("unknown param " + o[1].sym);
        writeDst(o[0], it->second & mk(t.bits));
        return;
      }
      u64 a = address(o.at(1));
      for (int i = 0; i < n; ++i) {
        u64 raw = env_.mem.read(a + i * bytes, bytes);
        u64 v = t.isSigned ? static_cast<u64>(sx(raw, t.bits)) : raw;
        if (n == 1) writeDst(o[0], v);
        else {
          if (o[0].kind != OpKind::Vec || static_cast<int>(o[0].regs.size()) != n) throw Unsupported("vector ld form");
          writeReg(o[0].regs[i], v);
        }
      }
      return;
    }
    if (ins.hasMod("param")) throw Unsupported("st.param");
    u64 a = address(o.at(0));
    for (int i = 0; i < n; ++i) {
      u64 v;
      if (n == 1) v = val(o.at(1), t);
      else {
        if (o.at(1).kind != OpKind::Vec || static_cast<int>(o[1].regs.size()) != n) throw Unsupported("vector st form");
        v = reg(o[1].regs[i]) & mk(t.bits);
      }
      env_.mem.write(a + i * bytes, bytes, v);
    }
  }
};

}  // namespace

ExecResult runThread(const Function& f, ExecEnv& env, long maxSteps) {
  Machine m(f, env);
  return m.run(maxSteps);
}

std::vector<ParamDesc> paramsOf(const Function& f) {
  std::vector<ParamDesc> out;
  auto toks = lex(f.header);
  for (size_t i = 0; i < toks.size(); ++i) {
    if (toks[i].text != ".param") continue;
    std::string type;
    size_t j = i + 1;
    while (j < toks.size() && toks[j].kind == Tok::Ident && toks[j].text[0] == '.') {
      if (toks[j].text == ".align") { j += 2; continue; }
      if (toks[j].text == ".ptr" || toks[j].text == ".global" || toks[j].text == ".const" ||
          toks[j].text == ".shared" || toks[j].text == ".local") { ++j; continue; }
      if (type.empty()) type = toks[j].text.substr(1);
      ++j;
    }
    if (j < toks.size() && toks[j].kind == Tok::Ident) out.push_back({toks[j].text, type});
  }
  return out;
}

DiffResult diffTest(const Function& a, const Function& b, int trials, unsigned seed) {
  DiffResult res;
  if (a.verbatim || b.verbatim) { res.status = DiffResult::Skipped; res.detail = "function kept verbatim"; return res; }
  auto params = paramsOf(a);
  for (int trial = 0; trial < trials; ++trial) {
    std::mt19937 rng(seed * 7919u + trial);
    ExecEnv base;
    int ptrIdx = 0;
    for (const auto& p : params) {
      TypeInfo t = parseType(p.type);
      if (!t.valid) { res.status = DiffResult::Skipped; res.detail = "unsupported parameter type ." + p.type; return res; }
      if (t.bits == 64) {
        u64 addr = 0x100000ULL * (++ptrIdx);
        for (int i = 0; i < 4096; ++i) base.mem.bytes[addr + i] = static_cast<uint8_t>(rng());
        base.params[p.name] = addr;
      } else if (t.isFloat) {
        base.params[p.name] = fromF(static_cast<float>(static_cast<int>(rng() % 1600) - 800) / 100.0f);
      } else {
        base.params[p.name] = rng() % 80;
      }
    }
    base.nctaid[0] = 2;
    base.ntid[0] = 32;
    ExecEnv ea = base, eb = base;
    for (uint32_t cta = 0; cta < 2; ++cta)
      for (uint32_t th = 0; th < 32; ++th) {
        for (ExecEnv* e : {&ea, &eb}) {
          e->ctaid[0] = cta;
          e->tid[0] = th;
          e->threadLinear = cta * 32 + th;
        }
        ExecResult ra = runThread(a, ea), rb = runThread(b, eb);
        if (ra.unsupported) { res.status = DiffResult::Skipped; res.detail = "interpreter: " + ra.error; return res; }
        if (!ra.ok && ra.error == "step limit exceeded") { res.status = DiffResult::Skipped; res.detail = "original does not terminate quickly"; return res; }
        if (ra.ok != rb.ok || (!rb.ok && ra.error != rb.error)) {
          res.status = DiffResult::Fail;
          res.detail = "trial " + std::to_string(trial) + ", thread " + std::to_string(cta * 32 + th) + ": outcome differs (" +
                       (ra.ok ? "ok" : ra.error) + " vs " + (rb.ok ? "ok" : rb.error) + ")";
          return res;
        }
      }
    if (ea.mem.bytes != eb.mem.bytes) {
      res.status = DiffResult::Fail;
      std::string where;
      for (const auto& kv : ea.mem.bytes) {
        auto it = eb.mem.bytes.find(kv.first);
        if (it == eb.mem.bytes.end() || it->second != kv.second) { where = " first difference at address 0x" ; char buf[32]; snprintf(buf, sizeof buf, "%llx", (unsigned long long)kv.first); where += buf; break; }
      }
      res.detail = "trial " + std::to_string(trial) + ": memory differs after execution" + where;
      return res;
    }
    ++res.trials;
  }
  return res;
}

}  // namespace ptx
