#include "ptxopt/dataflow.h"

namespace ptx {

Liveness computeLiveness(const Function& f, const Cfg& cfg) {
  Liveness lv;
  auto reg = [&](const std::string& r) {
    if (lv.id.find(r) == lv.id.end()) {
      lv.id[r] = static_cast<int>(lv.regs.size());
      lv.regs.push_back(r);
    }
  };
  for (const auto& it : f.items) {
    if (it.dead || it.kind != Item::Kind::Inst) continue;
    InstrInfo in = infoOf(it.ins);
    for (auto& r : in.defs) reg(r);
    for (auto& r : in.uses) reg(r);
    for (auto& r : in.clobbers) reg(r);
  }
  const size_t n = lv.regs.size(), nb = cfg.blocks.size();
  lv.use.assign(nb, BitSet(n));
  lv.def.assign(nb, BitSet(n));
  lv.in.assign(nb, BitSet(n));
  lv.out.assign(nb, BitSet(n));
  for (size_t b = 0; b < nb; ++b) {
    for (int i = cfg.blocks[b].begin; i < cfg.blocks[b].end; ++i) {
      const Item& it = f.items[i];
      if (it.dead || it.kind != Item::Kind::Inst) continue;
      InstrInfo in = infoOf(it.ins);
      for (auto& u : in.uses) {
        int k = lv.idOf(u);
        if (!lv.def[b].test(k)) lv.use[b].set(k);
      }
      if (it.ins.guard.empty())
        for (auto& d : in.defs) lv.def[b].set(lv.idOf(d));
    }
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t bi = nb; bi-- > 0;) {
      BitSet out(n);
      for (int s : cfg.blocks[bi].succ) out.unite(lv.in[s]);
      BitSet in = out;
      in.subtract(lv.def[bi]);
      in.unite(lv.use[bi]);
      if (in != lv.in[bi] || out != lv.out[bi]) {
        lv.in[bi] = in;
        lv.out[bi] = out;
        changed = true;
      }
    }
  }
  return lv;
}

void livenessTransfer(const Liveness& lv, const Instr& ins, BitSet& live) {
  InstrInfo in = infoOf(ins);
  if (ins.guard.empty())
    for (auto& d : in.defs) {
      int k = lv.idOf(d);
      if (k >= 0) live.reset(k);
    }
  for (auto& u : in.uses) {
    int k = lv.idOf(u);
    if (k >= 0) live.set(k);
  }
}

}  // namespace ptx
