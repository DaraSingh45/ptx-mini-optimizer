#include "ptxopt/analysis.h"

#include <algorithm>
#include <ostream>
#include <sstream>

#include "ptxopt/semantics.h"
#include "ptxopt/util.h"

namespace ptx {

namespace {
int slotWeight(const Function& f, const std::string& reg) {
  std::string t = f.regs.typeOf(reg);
  if (t == "pred") return 0;
  TypeInfo ti = parseType(t);
  return (ti.valid && ti.bits == 64) ? 2 : 1;
}
bool isPredReg(const Function& f, const std::string& reg) { return f.regs.typeOf(reg) == "pred"; }
}  // namespace

PressureInfo computePressure(const Function& f, const Cfg& cfg, const Liveness& lv) {
  PressureInfo p;
  p.perBlock.assign(cfg.blocks.size(), 0);
  auto measure = [&](const BitSet& live, int& slots, int& preds) {
    slots = preds = 0;
    live.forEach([&](size_t i) {
      const std::string& r = lv.regs[i];
      if (isPredReg(f, r)) ++preds;
      else slots += slotWeight(f, r);
    });
  };
  for (size_t b = 0; b < cfg.blocks.size(); ++b) {
    if (!cfg.reachable[b]) continue;
    BitSet live = lv.out[b];
    int line = 0;
    auto record = [&](int ln) {
      int s, pr;
      measure(live, s, pr);
      p.perBlock[b] = std::max(p.perBlock[b], s);
      if (s > p.maxSlots) {
        p.maxSlots = s;
        p.atBlock = static_cast<int>(b);
        p.atLine = ln;
      }
      p.maxPreds = std::max(p.maxPreds, pr);
    };
    for (int i = cfg.blocks[b].end - 1; i >= cfg.blocks[b].begin; --i) {
      const Item& it = f.items[i];
      if (it.dead || it.kind != Item::Kind::Inst) continue;
      line = it.ins.line;
      record(line);
      livenessTransfer(lv, it.ins, live);
    }
    record(line);
  }
  return p;
}

DivergenceInfo computeDivergence(const Function& f, const Cfg& cfg) {
  DivergenceInfo d;
  const int nb = static_cast<int>(cfg.blocks.size());
  bool changed = true;
  auto mark = [&](const std::string& r, const std::string& why) {
    if (d.varying.insert(r).second) {
      d.origin[r] = why;
      changed = true;
    }
  };
  while (changed) {
    changed = false;
    for (int b = 0; b < nb; ++b) {
      if (!cfg.reachable[b]) continue;
      for (int i = cfg.blocks[b].begin; i < cfg.blocks[b].end; ++i) {
        const Item& it = f.items[i];
        if (it.dead || it.kind != Item::Kind::Inst) continue;
        const Instr& ins = it.ins;
        InstrInfo in = infoOf(ins);
        std::string why;
        for (const auto& o : ins.ops)
          if (o.kind == OpKind::Special && (startsWith(o.text, "%tid") || o.text == "%laneid")) why = o.text;
        for (const auto& u : in.uses)
          if (d.varying.count(u)) { why = d.origin[u]; break; }
        if (why.empty() && (ins.op == "atom" || ins.op == "shfl" || (ins.op == "ld" && ins.hasMod("local"))))
          why = ins.full();
        if (why.empty() && d.regionBlocks.count(b)) why = "control dependence";
        if (!why.empty()) {
          for (const auto& r : in.defs) mark(r, why);
          for (const auto& r : in.clobbers) mark(r, why);
        }
      }
    }
    for (int b = 0; b < nb; ++b) {
      if (!cfg.reachable[b]) continue;
      int li = cfg.lastInstr(f, b);
      if (li < 0) continue;
      const Instr& t = f.items[li].ins;
      if (t.op != "bra" || t.guard.empty() || !d.varying.count(t.guard)) continue;
      int stop = cfg.ipdom.empty() ? -1 : cfg.ipdom[b];
      std::vector<int> stack(cfg.blocks[b].succ.begin(), cfg.blocks[b].succ.end());
      std::set<int> seen;
      while (!stack.empty()) {
        int x = stack.back();
        stack.pop_back();
        if (x == stop || !seen.insert(x).second) continue;
        if (d.regionBlocks.insert(x).second) changed = true;
        for (int s : cfg.blocks[x].succ) stack.push_back(s);
      }
    }
  }
  for (int b = 0; b < nb; ++b) {
    if (!cfg.reachable[b]) continue;
    int li = cfg.lastInstr(f, b);
    if (li < 0) continue;
    const Instr& t = f.items[li].ins;
    if (t.op == "bra" && !t.guard.empty() && d.varying.count(t.guard))
      d.branches.push_back({b, t.line, t.guard, d.origin[t.guard], squash(t.str())});
  }
  return d;
}

FuncStats analyzeFunction(const Function& f) {
  FuncStats s;
  s.name = f.name;
  s.verbatim = f.verbatim;
  s.verbatimReason = f.verbatimReason;
  if (f.verbatim) return s;
  s.instrs = countInstrs(f);
  Cfg cfg = buildCfg(f);
  if (!cfg.valid) {
    s.warnings.push_back("CFG invalid: " + cfg.error);
    return s;
  }
  s.blocks = static_cast<int>(cfg.blocks.size());
  for (const auto& b : cfg.blocks) s.edges += static_cast<int>(b.succ.size());
  s.loops = static_cast<int>(cfg.loops.size());
  for (int d : cfg.loopDepth) s.maxLoopDepth = std::max(s.maxLoopDepth, d);
  for (const auto& L : cfg.loops) {
    std::ostringstream o;
    o << "header B" << L.header << ", " << L.body.size() << " block(s), latch";
    for (int l : L.latches) o << " B" << l;
    s.loopDescriptions.push_back(o.str());
  }
  Liveness lv = computeLiveness(f, cfg);
  s.pressure = computePressure(f, cfg, lv);
  s.divergence = computeDivergence(f, cfg);

  std::set<std::string> opaqueOps;
  for (const auto& it : f.items) {
    if (it.dead || it.kind != Item::Kind::Inst) continue;
    ++s.histogram[it.ins.op];
    if (infoOf(it.ins).opaque) {
      ++s.opaque;
      opaqueOps.insert(it.ins.op);
    }
    for (const auto& o : it.ins.ops)
      if (o.kind == OpKind::Special && !isKnownSpecial(o.text))
        s.warnings.push_back("line " + std::to_string(it.ins.line) + ": '" + o.text +
                             "' is neither a declared register nor a known special register");
  }
  if (!opaqueOps.empty())
    s.warnings.push_back("opaque (not understood, left untouched): " +
                         join(std::vector<std::string>(opaqueOps.begin(), opaqueOps.end()), ", "));
  for (size_t i = 0; i < lv.regs.size(); ++i)
    if (!cfg.blocks.empty() && lv.in[0].test(i))
      s.warnings.push_back("register " + lv.regs[i] + " may be read before it is written");
  for (const auto& r : f.regs.ranges) s.declaredSlots += r.count * ((parseType(r.type).bits == 64) ? 2 : (r.type == "pred" ? 0 : 1));
  for (const auto& kv : f.regs.single) s.declaredSlots += (parseType(kv.second).bits == 64) ? 2 : (kv.second == "pred" ? 0 : 1);
  return s;
}

void printReport(std::ostream& os, const Module& m) {
  for (const auto& f : m.funcs) {
    FuncStats s = analyzeFunction(f);
    os << "== " << (f.isEntry ? "kernel " : "function ") << f.name << " ==\n";
    if (s.verbatim) {
      os << "  skipped (kept verbatim): " << s.verbatimReason << "\n\n";
      continue;
    }
    os << "  instructions : " << s.instrs << "\n";
    os << "  basic blocks : " << s.blocks << "   edges: " << s.edges << "\n";
    os << "  loops        : " << s.loops << "   max nesting: " << s.maxLoopDepth << "\n";
    for (const auto& l : s.loopDescriptions) os << "    - " << l << "\n";
    os << "  reg pressure : peak " << s.pressure.maxSlots << " x 32-bit slots, " << s.pressure.maxPreds
       << " predicates";
    if (s.pressure.atBlock >= 0) os << "  (block B" << s.pressure.atBlock << ", near line " << s.pressure.atLine << ")";
    os << "\n  declared regs: " << s.declaredSlots << " x 32-bit slots (declaration upper bound)\n";
    os << "  divergence   : ";
    if (s.divergence.branches.empty()) os << "no thread-divergent branches found\n";
    else {
      os << s.divergence.branches.size() << " potentially divergent branch(es)\n";
      for (const auto& b : s.divergence.branches)
        os << "    - line " << b.line << " in B" << b.block << ": " << b.text << "   [depends on " << b.origin << "]\n";
    }
    std::vector<std::pair<int, std::string>> h;
    for (const auto& kv : s.histogram) h.push_back({-kv.second, kv.first});
    std::sort(h.begin(), h.end());
    os << "  opcodes      : ";
    for (size_t i = 0; i < h.size(); ++i) os << (i ? ", " : "") << h[i].second << " x" << -h[i].first;
    os << "\n";
    for (const auto& w : s.warnings) os << "  warning      : " << w << "\n";
    os << "\n";
  }
}

void printJson(std::ostream& os, const Module& m) {
  os << "{\n  \"functions\": [\n";
  for (size_t i = 0; i < m.funcs.size(); ++i) {
    FuncStats s = analyzeFunction(m.funcs[i]);
    os << "    {\"name\": \"" << jsonEscape(s.name) << "\", \"skipped\": " << (s.verbatim ? "true" : "false")
       << ", \"instructions\": " << s.instrs << ", \"blocks\": " << s.blocks << ", \"loops\": " << s.loops
       << ", \"max_pressure_slots\": " << s.pressure.maxSlots << ", \"max_pressure_preds\": " << s.pressure.maxPreds
       << ", \"divergent_branches\": " << s.divergence.branches.size() << ", \"opaque\": " << s.opaque << "}"
       << (i + 1 < m.funcs.size() ? "," : "") << "\n";
  }
  os << "  ]\n}\n";
}

std::string toDot(const Function& f, bool showDom) {
  Cfg cfg = buildCfg(f);
  DivergenceInfo dv = computeDivergence(f, cfg);
  std::set<int> headers;
  for (const auto& L : cfg.loops) headers.insert(L.header);
  auto esc = [](const std::string& s) {
    std::string o;
    for (char c : s) {
      if (c == '"' || c == '\\' || c == '<' || c == '>' || c == '{' || c == '}' || c == '|') o += '\\';
      o += c;
    }
    return o;
  };
  std::ostringstream os;
  os << "digraph \"" << f.name << "\" {\n  node [shape=box, fontname=\"monospace\", fontsize=10];\n";
  for (size_t b = 0; b < cfg.blocks.size(); ++b) {
    std::string label = "B" + std::to_string(b);
    for (const auto& l : cfg.blocks[b].labels) label += " (" + l + ")";
    label += "\\l";
    for (int i = cfg.blocks[b].begin; i < cfg.blocks[b].end; ++i) {
      const Item& it = f.items[i];
      if (it.dead || it.kind != Item::Kind::Inst) continue;
      label += esc(squash(it.ins.str())) + "\\l";
    }
    std::string attrs;
    if (headers.count(static_cast<int>(b))) attrs += ", style=filled, fillcolor=\"#dbeafe\"";
    if (!cfg.reachable[b]) attrs += ", style=dashed";
    os << "  B" << b << " [label=\"" << label << "\"" << attrs << "];\n";
  }
  for (size_t b = 0; b < cfg.blocks.size(); ++b) {
    int li = cfg.lastInstr(f, static_cast<int>(b));
    bool div = false;
    for (const auto& br : dv.branches)
      if (br.block == static_cast<int>(b)) div = true;
    for (int s : cfg.blocks[b].succ) {
      std::string attr;
      if (li >= 0 && f.items[li].ins.op == "bra" && !f.items[li].ins.guard.empty()) {
        bool taken = f.items[li].ins.ops[0].text.size() && cfg.labelToBlock.count(f.items[li].ins.ops[0].text) &&
                     cfg.labelToBlock.at(f.items[li].ins.ops[0].text) == s;
        attr = std::string(" [label=\"") + (taken ? "taken" : "fall") + "\"" + (div ? ", color=red, fontcolor=red" : "") + "]";
      }
      os << "  B" << b << " -> B" << s << attr << ";\n";
    }
    if (showDom && cfg.idom.size() > b && cfg.idom[b] >= 0)
      os << "  B" << cfg.idom[b] << " -> B" << b << " [style=dashed, color=gray, arrowhead=empty];\n";
  }
  os << "}\n";
  return os.str();
}

}  // namespace ptx
