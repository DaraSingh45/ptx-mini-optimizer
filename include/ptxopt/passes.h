#pragma once
#include <map>
#include <string>
#include <vector>

#include "ptxopt/ir.h"

namespace ptx {

struct PassInfo {
  const char* name;
  const char* description;
  int (*run)(Function&);  // returns the number of changes made
};

// constfold, copyprop, cse, simplify, branch, dce  (in default pipeline order)
const std::vector<PassInfo>& allPasses();
std::vector<std::string> defaultPipeline();

struct OptStats {
  std::map<std::string, int> perPass;  // changes per pass
  int iterations = 0;
};

// Runs the passes repeatedly until nothing changes (or maxIter is reached).
OptStats optimizeFunction(Function& f, const std::vector<std::string>& passes, int maxIter = 10);

int passConstFold(Function& f);
int passCopyProp(Function& f);
int passCse(Function& f);
int passSimplify(Function& f);
int passBranch(Function& f);
int passDce(Function& f);

}  // namespace ptx
