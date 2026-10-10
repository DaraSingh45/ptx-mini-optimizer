#include "ptxopt/cfg.h"

#include <algorithm>
#include <functional>

#include "ptxopt/semantics.h"

namespace ptx {

namespace {

void addEdge(std::vector<BasicBlock>& bl, int a, int b) {
  auto& s = bl[a].succ;
  if (std::find(s.begin(), s.end(), b) == s.end()) {
    s.push_back(b);
    bl[b].pred.push_back(a);
  }
}

// Cooper-Harvey-Kennedy iterative dominator algorithm.
std::vector<int> computeIdom(int n, int entry, const std::vector<std::vector<int>>& succ,
                             const std::vector<std::vector<int>>& pred, std::vector<int>& rpoOut) {
  std::vector<int> post;
  std::vector<char> seen(n, 0);
  std::vector<std::pair<int, size_t>> st;
  st.push_back({entry, 0});
  seen[entry] = 1;
  while (!st.empty()) {
    int u = st.back().first;
    size_t& i = st.back().second;
    if (i < succ[u].size()) {
      int v = succ[u][i++];
      if (!seen[v]) {
        seen[v] = 1;
        st.push_back({v, 0});
      }
    } else {
      post.push_back(u);
      st.pop_back();
    }
  }
  std::vector<int> rpo(post.rbegin(), post.rend());
  std::vector<int> order(n, -1);
  for (size_t i = 0; i < rpo.size(); ++i) order[rpo[i]] = static_cast<int>(i);
  std::vector<int> idom(n, -1);
  idom[entry] = entry;
  bool changed = true;
  while (changed) {
    changed = false;
    for (int b : rpo) {
      if (b == entry) continue;
      int ni = -1;
      for (int p : pred[b]) {
        if (idom[p] == -1) continue;
        if (ni == -1) {
          ni = p;
        } else {
          int a = p, c = ni;
          while (a != c) {
            while (order[a] > order[c]) a = idom[a];
            while (order[c] > order[a]) c = idom[c];
          }
          ni = a;
        }
      }
      if (ni != idom[b]) {
        idom[b] = ni;
        changed = true;
      }
    }
  }
  idom[entry] = -1;
  rpoOut = rpo;
  return idom;
}

}  // namespace

int Cfg::lastInstr(const Function& f, int b) const {
  for (int i = blocks[b].end - 1; i >= blocks[b].begin; --i)
    if (!f.items[i].dead && f.items[i].kind == Item::Kind::Inst) return i;
  return -1;
}

bool dominates(const Cfg& c, int a, int b) {
  while (b != -1) {
    if (a == b) return true;
    b = c.idom[b];
  }
  return false;
}

Cfg buildCfg(const Function& f) {
  Cfg c;
  const int n = static_cast<int>(f.items.size());
  bool prevTerm = false, hasNonLabel = false;
  int cur = -1;
  for (int i = 0; i < n; ++i) {
    const Item& x = f.items[i];
    if (x.dead) continue;
    bool isLabel = x.kind == Item::Kind::Label;
    if (cur < 0 || prevTerm || (isLabel && hasNonLabel)) {
      c.blocks.push_back({});
      cur = static_cast<int>(c.blocks.size()) - 1;
      c.blocks[cur].begin = i;
      hasNonLabel = false;
      prevTerm = false;
    }
    c.blocks[cur].end = i + 1;
    if (isLabel) {
      c.blocks[cur].labels.push_back(x.text);
      c.labelToBlock[x.text] = cur;
    } else {
      hasNonLabel = true;
      if (x.kind == Item::Kind::Inst && isTerminator(x.ins)) prevTerm = true;
    }
  }
  const int nb = static_cast<int>(c.blocks.size());
  for (int b = 0; b < nb; ++b) {
    int li = c.lastInstr(f, b);
    bool fall = true;
    if (li >= 0 && isTerminator(f.items[li].ins)) {
      const Instr& t = f.items[li].ins;
      bool guarded = !t.guard.empty();
      if (t.op == "bra") {
        std::string target = t.ops.empty() ? "" : t.ops[0].text;
        auto it = c.labelToBlock.find(target);
        if (it == c.labelToBlock.end()) {
          c.valid = false;
          c.error = "branch to unknown label '" + target + "'";
        } else {
          addEdge(c.blocks, b, it->second);
        }
      }
      fall = guarded;
    }
    if (fall && b + 1 < nb) addEdge(c.blocks, b, b + 1);
  }
  c.reachable.assign(nb, false);
  c.loopDepth.assign(nb, 0);
  if (nb == 0) return c;

  std::vector<std::vector<int>> succ(nb), pred(nb);
  for (int b = 0; b < nb; ++b) {
    succ[b] = c.blocks[b].succ;
    pred[b] = c.blocks[b].pred;
  }
  c.idom = computeIdom(nb, 0, succ, pred, c.rpo);
  for (int b : c.rpo) c.reachable[b] = true;

  // Post-dominators on the reversed graph with a virtual exit node `nb`.
  std::vector<std::vector<int>> rs(nb + 1), rp(nb + 1);
  for (int b = 0; b < nb; ++b) {
    rs[b] = pred[b];
    rp[b] = succ[b];
    if (succ[b].empty()) {
      rs[nb].push_back(b);
      rp[b].push_back(nb);
    }
  }
  std::vector<int> rrpo;
  auto pd = computeIdom(nb + 1, nb, rs, rp, rrpo);
  c.ipdom.assign(pd.begin(), pd.begin() + nb);

  // Natural loops from back edges.
  std::map<int, Loop> byHeader;
  for (int u = 0; u < nb; ++u) {
    if (!c.reachable[u]) continue;
    for (int v : succ[u]) {
      if (!dominates(c, v, u)) continue;
      Loop& L = byHeader[v];
      L.header = v;
      L.latches.push_back(u);
    }
  }
  for (auto& kv : byHeader) {
    Loop& L = kv.second;
    std::vector<char> in(nb, 0);
    in[L.header] = 1;
    std::vector<int> stack(L.latches.begin(), L.latches.end());
    while (!stack.empty()) {
      int x = stack.back();
      stack.pop_back();
      if (in[x]) continue;
      in[x] = 1;
      for (int p : pred[x])
        if (c.reachable[p]) stack.push_back(p);
    }
    for (int b = 0; b < nb; ++b)
      if (in[b]) {
        L.body.push_back(b);
        ++c.loopDepth[b];
      }
    c.loops.push_back(L);
  }
  return c;
}

}  // namespace ptx
