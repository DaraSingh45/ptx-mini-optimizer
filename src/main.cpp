// ptx-opt: analyse and optimise NVIDIA PTX assembly.
#include <sys/stat.h>

#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "ptxopt/analysis.h"
#include "ptxopt/interp.h"
#include "ptxopt/parser.h"
#include "ptxopt/passes.h"
#include "ptxopt/util.h"

using namespace ptx;

namespace {

const char* kVersion = "ptx-opt 1.0";

void usage(std::ostream& os) {
  os << kVersion << " - PTX analyser and mini optimiser\n\n"
        "usage:\n"
        "  ptx-opt analyze  <file.ptx> [--json]\n"
        "  ptx-opt cfg      <file.ptx> --out <dir> [--dom]\n"
        "  ptx-opt optimize <file.ptx> [-o out.ptx] [--passes=a,b,c] [--stats] [--verify[=N]] [--max-iter=N]\n"
        "  ptx-opt verify   <original.ptx> <optimized.ptx> [--trials=N]\n"
        "  ptx-opt passes\n\n"
        "commands:\n"
        "  analyze   CFG, loops, register pressure, divergence report per function\n"
        "  cfg       write one Graphviz .dot file per function (render with: dot -Tpng f.dot -o f.png)\n"
        "  optimize  run optimisation passes; --verify differential-tests the result in the built-in interpreter\n"
        "  verify    differential-test two PTX files against each other\n"
        "  passes    list available passes\n";
}

std::vector<std::string> splitComma(const std::string& s) {
  std::vector<std::string> r;
  std::string cur;
  for (char c : s) {
    if (c == ',') { if (!cur.empty()) r.push_back(cur); cur.clear(); }
    else cur += c;
  }
  if (!cur.empty()) r.push_back(cur);
  return r;
}

bool hasOpt(const std::vector<std::string>& a, const std::string& o) {
  for (const auto& x : a) if (x == o) return true;
  return false;
}
std::string optValue(const std::vector<std::string>& a, const std::string& key, const std::string& def = "") {
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i] == key && i + 1 < a.size()) return a[i + 1];
    if (startsWith(a[i], key + "=")) return a[i].substr(key.size() + 1);
  }
  return def;
}

std::string safeName(const std::string& s) {
  std::string o;
  for (char c : s) o += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
  return o.substr(0, 100);
}

int cmdAnalyze(const std::vector<std::string>& args) {
  Module m = parseFile(args.at(0));
  if (hasOpt(args, "--json")) printJson(std::cout, m);
  else printReport(std::cout, m);
  return 0;
}

int cmdCfg(const std::vector<std::string>& args) {
  std::string dir = optValue(args, "--out", ".");
  mkdir(dir.c_str(), 0755);
  Module m = parseFile(args.at(0));
  for (const auto& f : m.funcs) {
    if (f.verbatim) { std::cerr << "skipping " << f.name << ": " << f.verbatimReason << "\n"; continue; }
    std::string path = dir + "/" + safeName(f.name) + ".dot";
    std::ofstream out(path);
    out << toDot(f, hasOpt(args, "--dom"));
    std::cout << "wrote " << path << "\n";
  }
  return 0;
}

void printVerify(const std::string& name, const DiffResult& r) {
  const char* s = r.status == DiffResult::Pass ? "PASS" : r.status == DiffResult::Fail ? "FAIL" : "SKIP";
  std::cout << "  verify " << name << ": " << s;
  if (r.status == DiffResult::Pass) std::cout << " (" << r.trials << " random trials x 64 threads, memory identical)";
  else std::cout << " - " << r.detail;
  std::cout << "\n";
}

int cmdOptimize(const std::vector<std::string>& args) {
  Module m = parseFile(args.at(0));
  Module orig = m;
  std::vector<std::string> passes = defaultPipeline();
  std::string pv = optValue(args, "--passes");
  if (!pv.empty()) {
    passes = splitComma(pv);
    for (const auto& p : passes) {
      bool ok = false;
      for (const auto& pi : allPasses()) if (p == pi.name) ok = true;
      if (!ok) { std::cerr << "unknown pass '" << p << "' (see: ptx-opt passes)\n"; return 1; }
    }
  }
  int maxIter = 10;
  std::string mi = optValue(args, "--max-iter");
  if (!mi.empty()) maxIter = std::stoi(mi);
  bool verify = false;
  int trials = 20;
  for (const auto& a : args)
    if (a == "--verify" || startsWith(a, "--verify=")) {
      verify = true;
      if (startsWith(a, "--verify=")) trials = std::stoi(a.substr(9));
    }
  bool stats = hasOpt(args, "--stats");
  bool anyFail = false;
  std::ostream& log = std::cerr;

  for (size_t i = 0; i < m.funcs.size(); ++i) {
    Function& f = m.funcs[i];
    if (f.verbatim) {
      if (stats) log << "== " << f.name << ": skipped (" << f.verbatimReason << ")\n";
      continue;
    }
    FuncStats before = analyzeFunction(f);
    OptStats os = optimizeFunction(f, passes, maxIter);
    FuncStats after = analyzeFunction(f);
    if (stats) {
      log << "== " << f.name << "\n";
      log << "  instructions : " << before.instrs << " -> " << after.instrs << "  ("
          << (before.instrs ? 100 * (before.instrs - after.instrs) / before.instrs : 0) << "% fewer)\n";
      log << "  basic blocks : " << before.blocks << " -> " << after.blocks << "\n";
      log << "  reg pressure : " << before.pressure.maxSlots << " -> " << after.pressure.maxSlots << " x 32-bit slots, "
          << before.pressure.maxPreds << " -> " << after.pressure.maxPreds << " predicates (peak live, estimated)\n";
      log << "  iterations   : " << os.iterations << "\n";
      for (const auto& p : passes) log << "  pass " << std::left << std::setw(10) << p << " " << os.perPass[p] << " change(s)\n";
    }
    if (verify) {
      DiffResult r = diffTest(orig.funcs[i], f, trials);
      printVerify(f.name, r);
      if (r.status == DiffResult::Fail) anyFail = true;
    }
  }
  std::string out = printModule(m);
  std::string path = optValue(args, "-o");
  if (path.empty()) std::cout << out;
  else {
    std::ofstream o(path);
    o << out;
    log << "wrote " << path << "\n";
  }
  return anyFail ? 2 : 0;
}

int cmdVerify(const std::vector<std::string>& args) {
  Module a = parseFile(args.at(0)), b = parseFile(args.at(1));
  int trials = 20;
  std::string t = optValue(args, "--trials");
  if (!t.empty()) trials = std::stoi(t);
  bool fail = false;
  for (const auto& fa : a.funcs)
    for (const auto& fb : b.funcs)
      if (fa.name == fb.name) {
        DiffResult r = diffTest(fa, fb, trials);
        printVerify(fa.name, r);
        if (r.status == DiffResult::Fail) fail = true;
      }
  return fail ? 2 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> a(argv + 1, argv + argc);
  if (a.empty() || a[0] == "-h" || a[0] == "--help" || a[0] == "help") { usage(std::cout); return a.empty() ? 1 : 0; }
  std::string cmd = a[0];
  a.erase(a.begin());
  try {
    if (cmd == "version" || cmd == "--version") { std::cout << kVersion << "\n"; return 0; }
    if (cmd == "passes") {
      for (const auto& p : allPasses()) std::cout << std::left << std::setw(10) << p.name << " " << p.description << "\n";
      return 0;
    }
    if (a.empty() || (cmd == "verify" && a.size() < 2)) { usage(std::cerr); return 1; }
    if (cmd == "analyze") return cmdAnalyze(a);
    if (cmd == "cfg") return cmdCfg(a);
    if (cmd == "optimize") return cmdOptimize(a);
    if (cmd == "verify") return cmdVerify(a);
    std::cerr << "unknown command '" << cmd << "'\n";
    usage(std::cerr);
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
