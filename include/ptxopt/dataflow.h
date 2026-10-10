#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "ptxopt/cfg.h"
#include "ptxopt/ir.h"
#include "ptxopt/semantics.h"
#include "ptxopt/util.h"

namespace ptx {

// Classic backward liveness over virtual registers.
struct Liveness {
  std::vector<std::string> regs;
  std::unordered_map<std::string, int> id;
  std::vector<BitSet> use, def, in, out;
  int idOf(const std::string& r) const {
    auto it = id.find(r);
    return it == id.end() ? -1 : it->second;
  }
};

Liveness computeLiveness(const Function& f, const Cfg& cfg);

// One backward step: live-before = (live-after - must-defs) U uses.
// Guarded definitions do not kill (the old value may survive).
void livenessTransfer(const Liveness& lv, const Instr& ins, BitSet& live);

}  // namespace ptx
