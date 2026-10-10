#pragma once
#include <map>
#include <string>
#include <vector>

#include "ptxopt/ir.h"

namespace ptx {

struct BasicBlock {
  int begin = 0, end = 0;  // item index range [begin, end) in Function::items
  std::vector<int> succ, pred;
  std::vector<std::string> labels;
};

struct Loop {
  int header = 0;
  std::vector<int> body;     // includes the header
  std::vector<int> latches;  // sources of back edges
};

struct Cfg {
  std::vector<BasicBlock> blocks;
  std::map<std::string, int> labelToBlock;
  std::vector<bool> reachable;
  std::vector<int> rpo;    // reverse post-order of reachable blocks
  std::vector<int> idom;   // immediate dominator (-1 for entry/unreachable)
  std::vector<int> ipdom;  // immediate post-dominator (-1 = none, blocks.size() = virtual exit)
  std::vector<Loop> loops;
  std::vector<int> loopDepth;
  bool valid = true;
  std::string error;

  int lastInstr(const Function& f, int block) const;  // item index of last live instruction, -1 if none
};

Cfg buildCfg(const Function& f);
bool dominates(const Cfg& c, int a, int b);

}  // namespace ptx
