#pragma once
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "ptxopt/ir.h"

namespace ptx {

// Single-thread functional interpreter for a PTX subset (integer/float ALU,
// predicates, branches, ld/st/ld.param). Used for differential testing:
// run the original and the optimised function on identical inputs and compare.
struct Unsupported : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct Memory {
  std::map<uint64_t, uint8_t> bytes;  // sparse, unwritten bytes read as 0
  uint64_t read(uint64_t addr, int n) const;
  void write(uint64_t addr, int n, uint64_t v);
};

struct ExecEnv {
  Memory mem;
  std::map<std::string, uint64_t> params;  // kernel parameter name -> value
  uint32_t tid[3] = {0, 0, 0}, ntid[3] = {1, 1, 1}, ctaid[3] = {0, 0, 0}, nctaid[3] = {1, 1, 1};
  uint64_t threadLinear = 0;  // selects the thread's private .local region
};

struct ExecResult {
  bool ok = true;
  bool unsupported = false;
  std::string error;
  long steps = 0;
};

ExecResult runThread(const Function& f, ExecEnv& env, long maxSteps = 200000);

struct DiffResult {
  enum Status { Pass, Fail, Skipped } status = Pass;
  std::string detail;
  int trials = 0;
};

// Runs `a` and `b` over a small grid with random parameters/memory and compares
// final memory and per-thread outcome.
DiffResult diffTest(const Function& a, const Function& b, int trials = 20, unsigned seed = 1);

struct ParamDesc {
  std::string name, type;
};
std::vector<ParamDesc> paramsOf(const Function& f);

}  // namespace ptx
