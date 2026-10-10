// Self-contained test runner (no external framework).  usage: ptx-opt-tests <samples-dir> [--fuzz=N]
#include <dirent.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>

#include "ptxopt/analysis.h"
#include "ptxopt/cfg.h"
#include "ptxopt/dataflow.h"
#include "ptxopt/interp.h"
#include "ptxopt/lexer.h"
#include "ptxopt/parser.h"
#include "ptxopt/passes.h"

using namespace ptx;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                              \
  do {                                                                           \
    if (cond) ++g_pass;                                                          \
    else { ++g_fail; std::cerr << "  CHECK FAILED " << __FILE__ << ":" << __LINE__ << "  " #cond "\n"; } \
  } while (0)
#define TEST(name) static void name(); static Registrar reg_##name(#name, name); static void name()

struct Registrar {
  Registrar(const char* n, void (*f)());
};
static std::vector<std::pair<std::string, void (*)()>>& tests() {
  static std::vector<std::pair<std::string, void (*)()>> t;
  return t;
}
Registrar::Registrar(const char* n, void (*f)()) { tests().push_back({n, f}); }

static std::string g_samples;

// ---- helpers
static const char* kHeader = ".version 7.8\n.target sm_75\n.address_size 64\n";
static std::string wrap(const std::string& body, const std::string& regs = ".reg .pred %p<4>;\n.reg .b32 %r<16>;\n.reg .b64 %rd<8>;\n") {
  return std::string(kHeader) + ".visible .entry k(.param .u64 k_param_0)\n{\n" + regs + body + "\n}\n";
}
static std::string optimizeText(const std::string& src, std::vector<std::string> passes = defaultPipeline()) {
  Module m = parseModule(src);
  for (auto& f : m.funcs) optimizeFunction(f, passes);
  return printModule(m);
}
static std::string readFileForTest(const std::string& path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
static bool contains(const std::string& s, const std::string& x) { return s.find(x) != std::string::npos; }

// ---- lexer / parser
TEST(lexer_basic) {
  auto t = lex("@%p1 bra $L__BB0_2; // c\n/* x */ ld.global.f32 %f1, [%rd1+8];");
  CHECK(t[0].text == "@" && t[0].kind == Tok::Punct);
  CHECK(t[1].text == "%p1" && t[2].text == "bra" && t[3].text == "$L__BB0_2");
  CHECK(t[4].text == ";");
  CHECK(t[5].text == "ld.global.f32");
  CHECK(t.back().kind == Tok::End);
  auto n = lex("0f3F800000 0x1F -5 1e-3");
  CHECK(n[0].kind == Tok::Number && n[0].text == "0f3F800000");
  CHECK(n[1].text == "0x1F");
  CHECK(n[2].text == "-" && n[3].text == "5");
  CHECK(n[4].text == "1e-3");
}

TEST(parse_vecadd) {
  Module m = parseFile(g_samples + "/vecadd.ptx");
  CHECK(m.funcs.size() == 1);
  const Function& f = m.funcs[0];
  CHECK(f.isEntry && f.name == "vecAdd" && !f.verbatim);
  CHECK(f.regs.typeOf("%rd10") == "b64");
  CHECK(f.regs.typeOf("%p1") == "pred");
  CHECK(!f.regs.declared("%rd11"));
  CHECK(countInstrs(f) == 22);
  Cfg c = buildCfg(f);
  CHECK(c.valid && c.blocks.size() == 3);
}

TEST(parse_line_directives_no_semicolon) {
  Module m = parseFile(g_samples + "/vecadd_O0.ptx");
  CHECK(m.funcs.size() == 1 && !m.funcs[0].verbatim);
  std::string out = printModule(m);
  CHECK(contains(out, ".loc 1 3 7"));
  CHECK(contains(out, ".file"));
  CHECK(contains(out, ".version 7.8"));
}

TEST(roundtrip_is_stable) {
  for (const char* n : {"vecadd", "vecadd_O0", "saxpy_loop", "divergent", "reduce", "redundant"}) {
    Module a = parseFile(g_samples + "/" + n + ".ptx");
    std::string p1 = printModule(a);
    std::string p2 = printModule(parseModule(p1));
    CHECK(p1 == p2);
  }
}

TEST(nested_scope_kept_verbatim) {
  Module m = parseFile(g_samples + "/nested_scope.ptx");
  CHECK(m.funcs.size() == 1 && m.funcs[0].verbatim);
  std::string out = optimizeText(readFileForTest(g_samples + "/nested_scope.ptx"));
  CHECK(contains(out, ".reg .b32 temp;"));
}

TEST(unknown_instruction_is_opaque_and_preserved) {
  std::string src = wrap("mov.u32 %r1, 5;\nfoo.bar.u32 %r2, %r1;\nst.global.u32 [%rd1], %r2;\nret;");
  Module m = parseModule(src);
  CHECK(!m.funcs[0].verbatim);
  std::string out = optimizeText(src);
  CHECK(contains(out, "foo.bar.u32\t%r2, %r1"));
  CHECK(contains(out, "mov.u32\t%r1, 5"));  // still needed: the opaque op reads %r1
}

// ---- passes
TEST(constant_folding_and_dce) {
  std::string out = optimizeText(wrap(
      "mov.u32 %r1, 5;\nmov.u32 %r2, 3;\nadd.s32 %r3, %r1, %r2;\nmul.lo.s32 %r4, %r3, 2;\nst.global.u32 [%rd1], %r4;\nret;"));
  CHECK(contains(out, "st.global.u32\t[%rd1], 16") || contains(out, "mov.s32\t%r4, 16") || contains(out, "mov.u32\t%r4, 16"));
  CHECK(!contains(out, "add.s32"));
}

TEST(strength_reduction) {
  std::string out = optimizeText(wrap(
      "mov.u32 %r1, %tid.x;\nmul.lo.s32 %r2, %r1, 8;\ndiv.u32 %r3, %r1, 4;\nrem.u32 %r4, %r1, 16;\n"
      "st.global.u32 [%rd1], %r2;\nst.global.u32 [%rd1+4], %r3;\nst.global.u32 [%rd1+8], %r4;\nret;"));
  CHECK(contains(out, "shl.b32\t%r2, %r1, 3"));
  CHECK(contains(out, "shr.u32\t%r3, %r1, 2"));
  CHECK(contains(out, "and.b32\t%r4, %r1, 15"));
}

TEST(identities) {
  std::string out = optimizeText(wrap(
      "mov.u32 %r1, %tid.x;\nadd.s32 %r2, %r1, 0;\nmul.lo.s32 %r3, %r2, 1;\nor.b32 %r4, %r3, 0;\n"
      "st.global.u32 [%rd1], %r4;\nret;"));
  CHECK(contains(out, "st.global.u32\t[%rd1], %r1"));
  CHECK(!contains(out, "add.s32") && !contains(out, "mul.lo") && !contains(out, "or.b32"));
}

TEST(constant_branch_and_unreachable_block) {
  std::string out = optimizeText(wrap(
      "mov.u32 %r1, 7;\nsetp.lt.s32 %p1, %r1, 5;\n@%p1 bra $L_dead;\nst.global.u32 [%rd1], %r1;\nret;\n"
      "$L_dead:\nmov.u32 %r2, 99;\nst.global.u32 [%rd1], %r2;\nret;"));
  CHECK(!contains(out, "$L_dead"));
  CHECK(!contains(out, "99"));
  CHECK(!contains(out, "setp"));
}

TEST(taken_constant_branch_becomes_goto) {
  std::string out = optimizeText(wrap(
      "mov.u32 %r1, 3;\nsetp.lt.s32 %p1, %r1, 5;\n@%p1 bra $L_t;\nmov.u32 %r2, 1;\nst.global.u32 [%rd1], %r2;\nret;\n"
      "$L_t:\nmov.u32 %r3, 2;\nst.global.u32 [%rd1], %r3;\nret;"));
  CHECK(!contains(out, "bra"));   // jump to the next block is removed too
  CHECK(contains(out, "mov.u32\t%r3, 2"));    // the taken path survives
  CHECK(!contains(out, "mov.u32\t%r2, 1"));   // the skipped path is gone
}

TEST(cse_removes_duplicate_computation) {
  std::string out = optimizeText(wrap(
      "mov.u32 %r1, %tid.x;\nmov.u32 %r2, %ntid.x;\nmul.lo.s32 %r3, %r1, %r2;\nmul.lo.s32 %r4, %r2, %r1;\n"
      "add.s32 %r5, %r3, %r4;\nst.global.u32 [%rd1], %r5;\nret;"));
  int n = 0;
  for (size_t p = out.find("mul.lo.s32"); p != std::string::npos; p = out.find("mul.lo.s32", p + 1)) ++n;
  CHECK(n == 1);
}

TEST(copy_propagation_across_blocks_single_def) {
  std::string out = optimizeText(wrap(
      "mov.u32 %r1, %tid.x;\nmov.u32 %r2, %r1;\nsetp.ge.s32 %p1, %r1, 4;\n@%p1 bra $L_x;\nst.global.u32 [%rd1], %r2;\n$L_x:\nret;"));
  CHECK(contains(out, "[%rd1], %r1"));
  CHECK(!contains(out, "mov.u32\t%r2, %r1"));
}

TEST(redefinition_blocks_copy_propagation) {
  // %r1 is redefined after the copy, so the copy of the OLD value must not be replaced by %r1.
  std::string src = wrap(
      "mov.u32 %r1, %tid.x;\nmov.u32 %r2, %r1;\nadd.s32 %r1, %r1, 1;\nst.global.u32 [%rd1], %r2;\nst.global.u32 [%rd1+4], %r1;\nret;");
  std::string out = optimizeText(src);
  CHECK(contains(out, "[%rd1], %r2"));
}

TEST(guarded_definition_does_not_kill) {
  // @p mov may or may not execute; the earlier definition must stay.
  std::string src = wrap(
      "mov.u32 %r1, 10;\nmov.u32 %r2, %tid.x;\nsetp.eq.s32 %p1, %r2, 0;\n@%p1 mov.u32 %r1, 20;\nst.global.u32 [%rd1], %r1;\nret;");
  std::string out = optimizeText(src);
  CHECK(contains(out, "mov.u32\t%r1, 10"));
  CHECK(contains(out, "@%p1 mov.u32\t%r1, 20"));
}

TEST(side_effects_are_never_removed) {
  std::string out = optimizeText(wrap(
      "mov.u32 %r1, 1;\natom.global.add.u32 %r2, [%rd1], %r1;\nst.global.u32 [%rd1+4], %r1;\nbar.sync 0;\nret;"));
  CHECK(contains(out, "atom.global.add.u32"));
  CHECK(contains(out, "bar.sync"));
}

TEST(volatile_load_is_kept) {
  std::string out = optimizeText(wrap("ld.volatile.global.u32 %r1, [%rd1];\nret;"));
  CHECK(contains(out, "ld.volatile.global.u32"));
  out = optimizeText(wrap("ld.global.u32 %r1, [%rd1];\nret;"));
  CHECK(!contains(out, "ld.global"));
}

TEST(optimizer_is_idempotent) {
  for (const char* n : {"redundant", "vecadd_O0", "vecadd", "saxpy_loop", "divergent"}) {
    Module m = parseFile(g_samples + "/" + n + ".ptx");
    for (auto& f : m.funcs) optimizeFunction(f, defaultPipeline());
    std::string once = printModule(m);
    Module m2 = parseModule(once);
    int changes = 0;
    for (auto& f : m2.funcs) {
      OptStats s = optimizeFunction(f, defaultPipeline());
      for (auto& kv : s.perPass) changes += kv.second;
    }
    CHECK(changes == 0);
  }
}

// ---- analyses
TEST(cfg_loops_and_dominators) {
  Module m = parseFile(g_samples + "/saxpy_loop.ptx");
  Cfg c = buildCfg(m.funcs[0]);
  CHECK(c.loops.size() == 1);
  CHECK(c.loops[0].header == 2 && c.loops[0].latches.size() == 1 && c.loops[0].latches[0] == 2);
  CHECK(dominates(c, 0, 2) && dominates(c, 2, 2) && !dominates(c, 2, 0));
  CHECK(c.loopDepth[2] == 1 && c.loopDepth[0] == 0);
  CHECK(c.idom[2] == 1);
}

TEST(liveness_basic) {
  Module m = parseModule(wrap("mov.u32 %r1, 1;\nmov.u32 %r2, 2;\nadd.s32 %r3, %r1, %r2;\nst.global.u32 [%rd1], %r3;\nret;"));
  const Function& f = m.funcs[0];
  Cfg c = buildCfg(f);
  Liveness lv = computeLiveness(f, c);
  CHECK(lv.in[0].test(lv.idOf("%rd1")));     // read before written
  CHECK(!lv.in[0].test(lv.idOf("%r1")));
  CHECK(lv.out[0].count() == 0);
}

TEST(divergence_analysis) {
  Module m = parseFile(g_samples + "/divergent.ptx");
  FuncStats s = analyzeFunction(m.funcs[0]);
  CHECK(s.divergence.branches.size() == 1);
  CHECK(s.divergence.branches[0].origin == "%tid.x");
  CHECK(s.divergence.varying.count("%r6"));   // value depends on a divergent branch
  CHECK(!s.divergence.varying.count("%p2"));  // uniform predicate (kernel parameter)
}

TEST(register_pressure_drops_after_optimization) {
  Module m = parseFile(g_samples + "/redundant.ptx");
  int before = analyzeFunction(m.funcs[0]).pressure.maxSlots;
  optimizeFunction(m.funcs[0], defaultPipeline());
  int after = analyzeFunction(m.funcs[0]).pressure.maxSlots;
  CHECK(after < before);
}

// ---- differential testing on all samples
TEST(samples_optimize_and_verify) {
  DIR* d = opendir(g_samples.c_str());
  CHECK(d != nullptr);
  if (!d) return;
  std::vector<std::string> files;
  while (dirent* e = readdir(d)) {
    std::string n = e->d_name;
    if (n.size() > 4 && n.substr(n.size() - 4) == ".ptx") files.push_back(n);
  }
  closedir(d);
  std::sort(files.begin(), files.end());
  CHECK(files.size() >= 6);
  for (const auto& n : files) {
    Module orig = parseFile(g_samples + "/" + n);
    Module opt = orig;
    for (auto& f : opt.funcs) optimizeFunction(f, defaultPipeline());
    for (size_t i = 0; i < orig.funcs.size(); ++i) {
      DiffResult r = diffTest(orig.funcs[i], opt.funcs[i], 12);
      if (r.status == DiffResult::Fail) std::cerr << "  " << n << ": " << r.detail << "\n";
      CHECK(r.status != DiffResult::Fail);
    }
  }
}

// ---- fuzzing: random well-defined programs, optimise, differential-test ----
namespace {
struct Gen {
  std::mt19937 rng;
  explicit Gen(unsigned seed) : rng(seed) {}
  int pick(int n) { return static_cast<int>(rng() % n); }
  std::string imm() {
    static const char* v[] = {"0", "1", "2", "3", "4", "7", "8", "16", "255", "-1", "-2", "100", "2147483647"};
    return v[pick(13)];
  }
  std::string program() {
    std::vector<std::string> defined;       // definitely-defined registers on the current path
    std::vector<std::string> body;
    int nextReg = 1, nextPred = 1, nextLabel = 0;
    struct Pending { std::string label; int at; std::vector<std::string> snapshot; };
    std::vector<Pending> pending;
    int n = 14 + pick(24);
    auto newReg = [&]() { return "%r" + std::to_string(nextReg++); };
    // seeds: thread-dependent values so that nothing is trivially constant
    body.push_back("mov.u32 %r0, %tid.x;");
    defined.push_back("%r0");
    auto src = [&]() { return defined[pick(defined.size())]; };
    auto ty = [&]() { return pick(2) ? "s32" : "u32"; };
    for (int i = 0; i < n && nextReg < 60; ++i) {
      for (size_t k = 0; k < pending.size();) {
        if (pending[k].at <= i) {
          body.push_back(pending[k].label + ":");
          std::vector<std::string> inter;
          for (auto& r : defined)
            if (std::find(pending[k].snapshot.begin(), pending[k].snapshot.end(), r) != pending[k].snapshot.end()) inter.push_back(r);
          defined = inter;
          pending.erase(pending.begin() + k);
        } else ++k;
      }
      int kind = pick(15);
      std::string d = newReg();
      std::ostringstream o;
      if (kind == 0) { o << "mov.u32 " << d << ", " << imm() << ";"; defined.push_back(d); }
      else if (kind == 1) { o << "mov.u32 " << d << ", " << src() << ";"; defined.push_back(d); }
      else if (kind <= 5) {
        static const char* ops[] = {"add", "sub", "and", "or", "xor", "min", "max"};
        std::string op = ops[pick(7)];
        std::string t = (op == "and" || op == "or" || op == "xor") ? "b32" : ty();
        o << op << "." << t << " " << d << ", " << src() << ", " << (pick(2) ? imm() : src()) << ";";
        defined.push_back(d);
      } else if (kind == 6) { o << "mul.lo." << ty() << " " << d << ", " << src() << ", " << (pick(2) ? imm() : src()) << ";"; defined.push_back(d); }
      else if (kind == 7) { o << "mad.lo." << ty() << " " << d << ", " << src() << ", " << (pick(2) ? imm() : src()) << ", " << (pick(2) ? imm() : src()) << ";"; defined.push_back(d); }
      else if (kind == 8) { o << (pick(2) ? "shl.b32 " : (pick(2) ? "shr.u32 " : "shr.s32 ")) << d << ", " << src() << ", " << pick(34) << ";"; defined.push_back(d); }
      else if (kind == 9) { static const char* dv[] = {"1", "2", "3", "8", "16", "7"}; o << (pick(2) ? "div.u32 " : "rem.u32 ") << d << ", " << src() << ", " << dv[pick(6)] << ";"; defined.push_back(d); }
      else if (kind == 10) { o << (pick(2) ? "neg.s32 " : "not.b32 ") << d << ", " << src() << ";"; defined.push_back(d); }
      else if (kind == 11) { o << "cvt.u32.u32 " << d << ", " << src() << ";"; defined.push_back(d); }
      else if (kind == 12) {  // predicated update of an already-defined register
        static const char* cmp[] = {"eq", "ne", "lt", "le", "gt", "ge"};
        std::string p = "%p" + std::to_string(nextPred++ % 4);
        body.push_back(std::string("setp.") + cmp[pick(6)] + "." + ty() + " " + p + ", " + src() + ", " + (pick(2) ? imm() : src()) + ";");
        std::string target = src();
        if (pick(2)) o << "@" << (pick(2) ? "" : "!") << p << " add.s32 " << target << ", " << target << ", " << imm() << ";";
        else o << "@" << (pick(2) ? "" : "!") << p << " mov.u32 " << target << ", " << (pick(2) ? imm() : src()) << ";";  // overwrites without reading
        --nextReg;
      } else if (kind == 13 && nextLabel < 3) {  // forward conditional branch
        static const char* cmp[] = {"eq", "ne", "lt", "ge"};
        std::string p = "%p" + std::to_string(nextPred++ % 4);
        std::string lab = "$L_" + std::to_string(nextLabel++);
        body.push_back(std::string("setp.") + cmp[pick(4)] + "." + ty() + " " + p + ", " + src() + ", " + (pick(2) ? imm() : src()) + ";");
        o << "@" << p << " bra " << lab << ";";
        pending.push_back({lab, i + 1 + pick(6), defined});
        --nextReg;
      } else { o << "add.s32 " << d << ", " << src() << ", " << src() << ";"; defined.push_back(d); }
      body.push_back(o.str());
    }
    for (auto& p : pending) {
      body.push_back(p.label + ":");
      std::vector<std::string> inter;
      for (auto& r : defined)
        if (std::find(p.snapshot.begin(), p.snapshot.end(), r) != p.snapshot.end()) inter.push_back(r);
      defined = inter;
    }
    std::ostringstream tail;
    tail << "ld.param.u64 %rd1, [k_param_0];\ncvta.to.global.u64 %rd2, %rd1;\nmul.wide.u32 %rd3, %r0, 128;\nadd.s64 %rd4, %rd2, %rd3;\n";
    int off = 0;
    for (auto& r : defined) { tail << "st.global.u32 [%rd4+" << off << "], " << r << ";\n"; off += 4; if (off > 120) break; }
    tail << "ret;";
    std::string text;
    for (auto& l : body) text += l + "\n";
    return wrap(text + tail.str(), ".reg .pred %p<4>;\n.reg .b32 %r<64>;\n.reg .b64 %rd<8>;\n");
  }
};
}  // namespace

static int g_fuzz = 400;

TEST(fuzz_random_programs) {
  int skipped = 0, failed = 0;
  for (int seed = 1; seed <= g_fuzz; ++seed) {
    Gen g(seed);
    std::string src = g.program();
    Module orig = parseModule(src);
    CHECK(orig.funcs.size() == 1 && !orig.funcs[0].verbatim);
    Module opt = orig;
    optimizeFunction(opt.funcs[0], defaultPipeline());
    // the optimised program must also reparse
    Module re = parseModule(printModule(opt));
    CHECK(!re.funcs[0].verbatim);
    DiffResult r = diffTest(orig.funcs[0], opt.funcs[0], 3, seed);
    if (r.status == DiffResult::Skipped) ++skipped;
    if (r.status == DiffResult::Fail) {
      ++failed;
      std::cerr << "  fuzz seed " << seed << ": " << r.detail << "\n";
      if (failed == 1) std::cerr << "---- original ----\n" << src << "---- optimised ----\n" << printModule(opt) << "\n";
    }
    CHECK(r.status != DiffResult::Fail);
  }
  std::cout << "  fuzz: " << g_fuzz << " programs, " << skipped << " skipped, " << failed << " failed\n";
}

int main(int argc, char** argv) {
  if (argc < 2) { std::cerr << "usage: ptx-opt-tests <samples-dir> [--fuzz=N]\n"; return 2; }
  g_samples = argv[1];
  for (int i = 2; i < argc; ++i)
    if (std::string(argv[i]).rfind("--fuzz=", 0) == 0) g_fuzz = std::stoi(std::string(argv[i]).substr(7));
  for (auto& t : tests()) {
    int before = g_fail;
    std::cout << "[ RUN  ] " << t.first << "\n";
    try { t.second(); } catch (const std::exception& e) { ++g_fail; std::cerr << "  exception: " << e.what() << "\n"; }
    std::cout << (g_fail == before ? "[  OK  ] " : "[ FAIL ] ") << t.first << "\n";
  }
  std::cout << "\n" << g_pass << " checks passed, " << g_fail << " failed\n";
  return g_fail ? 1 : 0;
}
