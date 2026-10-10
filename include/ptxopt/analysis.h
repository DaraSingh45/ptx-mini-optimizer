#pragma once
#include <iosfwd>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ptxopt/cfg.h"
#include "ptxopt/dataflow.h"
#include "ptxopt/ir.h"

namespace ptx {

struct PressureInfo {
  int maxSlots = 0;  // peak live 32-bit register slots (64-bit values count 2)
  int maxPreds = 0;  // peak live predicate registers
  int atBlock = -1, atLine = 0;
  std::vector<int> perBlock;
};

struct DivergentBranch {
  int block = 0, line = 0;
  std::string pred, origin, text;
};

struct DivergenceInfo {
  std::set<std::string> varying;
  std::map<std::string, std::string> origin;
  std::vector<DivergentBranch> branches;
  std::set<int> regionBlocks;  // blocks control-dependent on a divergent branch
};

struct FuncStats {
  std::string name;
  bool verbatim = false;
  std::string verbatimReason;
  int instrs = 0, blocks = 0, edges = 0, loops = 0, maxLoopDepth = 0, opaque = 0;
  int declaredSlots = 0;
  PressureInfo pressure;
  DivergenceInfo divergence;
  std::map<std::string, int> histogram;
  std::vector<std::string> warnings;
  std::vector<std::string> loopDescriptions;
};

PressureInfo computePressure(const Function& f, const Cfg& cfg, const Liveness& lv);
DivergenceInfo computeDivergence(const Function& f, const Cfg& cfg);
FuncStats analyzeFunction(const Function& f);

void printReport(std::ostream& os, const Module& m);
void printJson(std::ostream& os, const Module& m);
std::string toDot(const Function& f, bool showDominators);

}  // namespace ptx
