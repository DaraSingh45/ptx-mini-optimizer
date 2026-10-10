#include "ptxopt/parser.h"

#include <cctype>
#include <fstream>
#include <sstream>

#include "ptxopt/lexer.h"
#include "ptxopt/semantics.h"
#include "ptxopt/util.h"

namespace ptx {

namespace {

class Parser {
 public:
  explicit Parser(const std::string& src) : src_(src), toks_(lex(src)) {}

  Module run() {
    Module m;
    while (!atEnd()) {
      if (isFuncStart()) parseFunction(m);
      else parseTopRaw(m);
    }
    return m;
  }

 private:
  const std::string& src_;
  std::vector<Token> toks_;
  size_t pos_ = 0;

  bool atEnd() const { return toks_[pos_].kind == Tok::End; }
  bool isP(size_t i, const char* s) const { return toks_[i].kind == Tok::Punct && toks_[i].text == s; }
  std::string slice(size_t a, size_t b) const {  // tokens [a, b]
    return src_.substr(toks_[a].begin, toks_[b].end - toks_[a].begin);
  }

  bool isFuncStart() const {
    size_t j = pos_;
    while (toks_[j].kind == Tok::Ident &&
           (toks_[j].text == ".visible" || toks_[j].text == ".extern" || toks_[j].text == ".weak" ||
            toks_[j].text == ".common" || toks_[j].text == ".noreturn"))
      ++j;
    return toks_[j].text == ".entry" || toks_[j].text == ".func";
  }

  void parseTopRaw(Module& m) {
    size_t start = pos_;
    const Token& t = toks_[pos_];
    bool lineStmt = t.text == ".version" || t.text == ".target" || t.text == ".address_size" ||
                    t.text == ".file" || t.text == ".loc";
    if (lineStmt) {
      int ln = t.line;
      ++pos_;
      while (!atEnd() && toks_[pos_].line == ln) ++pos_;
    } else {
      int depth = 0;
      bool isSection = t.text == ".section";
      while (!atEnd()) {
        size_t i = pos_++;
        if (isP(i, "{")) ++depth;
        else if (isP(i, "}")) {
          --depth;
          if (depth <= 0 && isSection) break;
        } else if (isP(i, ";") && depth == 0) break;
      }
    }
    TopItem ti;
    ti.raw = trim(slice(start, pos_ - 1));
    m.top.push_back(ti);
  }

  void skipParens() {
    int depth = 0;
    while (!atEnd()) {
      if (isP(pos_, "(")) ++depth;
      else if (isP(pos_, ")")) {
        --depth;
        if (depth == 0) { ++pos_; return; }
      }
      ++pos_;
    }
  }

  void parseFunction(Module& m) {
    size_t start = pos_;
    while (toks_[pos_].text == ".visible" || toks_[pos_].text == ".extern" || toks_[pos_].text == ".weak" ||
           toks_[pos_].text == ".common" || toks_[pos_].text == ".noreturn")
      ++pos_;
    bool isEntry = toks_[pos_].text == ".entry";
    ++pos_;
    if (!isEntry && isP(pos_, "(")) skipParens();
    std::string name = toks_[pos_].text;
    ++pos_;
    if (isP(pos_, "(")) skipParens();
    while (!atEnd() && !isP(pos_, "{") && !isP(pos_, ";")) ++pos_;
    if (atEnd()) throw ParseError("unexpected end of file in function " + name);
    if (isP(pos_, ";")) {  // prototype / extern declaration
      TopItem ti;
      ti.raw = trim(slice(start, pos_));
      ++pos_;
      m.top.push_back(ti);
      return;
    }
    size_t brace = pos_, close = 0;
    int depth = 0;
    bool nested = false;
    for (size_t j = brace; j < toks_.size(); ++j) {
      if (isP(j, "{")) {
        ++depth;
        if (depth > 1) nested = true;
      } else if (isP(j, "}")) {
        if (--depth == 0) { close = j; break; }
      }
    }
    if (close == 0) throw ParseError("unterminated function body: " + name);

    Function f;
    f.name = name;
    f.isEntry = isEntry;
    f.line = toks_[start].line;
    f.header = trim(src_.substr(toks_[start].begin, toks_[brace].begin - toks_[start].begin));
    f.rawText = src_.substr(toks_[start].begin, toks_[close].end - toks_[start].begin);
    pos_ = close + 1;
    if (nested) {
      f.verbatim = true;
      f.verbatimReason = "nested { } scope (inline asm / call frames)";
    } else {
      try {
        parseBody(brace + 1, close, f);
        for (const auto& it : f.items)
          if (it.kind == Item::Kind::Inst && it.ins.op == "brx") {
            f.verbatim = true;
            f.verbatimReason = "indirect branch (brx.idx)";
          }
      } catch (const ParseError& e) {
        f.verbatim = true;
        f.verbatimReason = std::string("parse error: ") + e.what();
      }
      if (f.verbatim) {
        f.items.clear();
        f.decls.clear();
      }
    }
    TopItem ti;
    ti.isFunc = true;
    ti.func = m.funcs.size();
    m.top.push_back(ti);
    m.funcs.push_back(std::move(f));
  }

  void parseRegDecl(size_t i, size_t j, Function& f) {
    std::string type;
    while (i < j) {
      const Token& t = toks_[i];
      if (t.kind == Tok::Ident && t.text[0] == '.') {
        if (t.text == ".align") { i += 2; continue; }
        if (t.text == ".v2" || t.text == ".v4" || t.text == ".v8") { ++i; continue; }
        if (type.empty()) type = t.text.substr(1);
        ++i;
        continue;
      }
      if (t.kind == Tok::Ident) {
        std::string name = t.text;
        ++i;
        if (i < j && isP(i, "<") && i + 2 < j + 1) {
          int count = std::stoi(toks_[i + 1].text);
          i += 3;
          f.regs.ranges.push_back({name, count, type});
        } else {
          f.regs.single[name] = type;
        }
        if (i < j && isP(i, ",")) ++i;
        continue;
      }
      ++i;
    }
  }

  Operand parseOperand(size_t& k, size_t end, const RegDecls& rd) {
    if (k >= end) throw ParseError("missing operand");
    const Token& t = toks_[k];
    Operand o;
    if (isP(k, "{")) {
      ++k;
      o.kind = OpKind::Vec;
      while (true) {
        if (k >= end || toks_[k].kind != Tok::Ident || !rd.declared(toks_[k].text))
          throw ParseError("unsupported vector element");
        o.regs.push_back(toks_[k].text);
        ++k;
        if (k < end && isP(k, ",")) { ++k; continue; }
        if (k < end && isP(k, "}")) { ++k; break; }
        throw ParseError("bad vector operand");
      }
      return o;
    }
    if (isP(k, "[")) {
      ++k;
      o.kind = OpKind::Mem;
      if (k >= end) throw ParseError("bad memory operand");
      const Token& b = toks_[k];
      if (b.kind == Tok::Ident) {
        if (rd.declared(b.text)) o.regs.push_back(b.text);
        else o.sym = b.text;
      } else if (b.kind == Tok::Number) {
        o.text = b.text;
      } else {
        throw ParseError("bad memory operand");
      }
      ++k;
      if (k < end && (isP(k, "+") || isP(k, "-"))) {
        std::string sign = toks_[k].text;
        ++k;
        if (k >= end || toks_[k].kind != Tok::Number) throw ParseError("bad memory offset");
        o.text += sign + toks_[k].text;
        ++k;
      }
      if (k >= end || !isP(k, "]")) throw ParseError("expected ]");
      ++k;
      return o;
    }
    if (isP(k, "-") || isP(k, "+")) {
      if (k + 1 >= end || toks_[k + 1].kind != Tok::Number) throw ParseError("bad signed operand");
      o.kind = OpKind::Imm;
      o.text = (t.text == "-" ? "-" : "") + toks_[k + 1].text;
      k += 2;
      return o;
    }
    if (t.kind == Tok::Number) {
      o.kind = OpKind::Imm;
      o.text = t.text;
      ++k;
      return o;
    }
    if (t.kind == Tok::Ident) {
      if (rd.declared(t.text)) {
        o.kind = OpKind::Reg;
        o.regs.push_back(t.text);
        ++k;
        if (k < end && isP(k, "|")) {
          ++k;
          if (k >= end || toks_[k].kind != Tok::Ident || !rd.declared(toks_[k].text))
            throw ParseError("bad predicate pair");
          o.kind = OpKind::Pair;
          o.regs.push_back(toks_[k].text);
          ++k;
        }
      } else if (t.text[0] == '%') {
        o.kind = OpKind::Special;
        o.text = t.text;
        ++k;
      } else {
        o.kind = OpKind::Sym;
        o.text = t.text;
        ++k;
      }
      return o;
    }
    throw ParseError("unsupported operand '" + t.text + "'");
  }

  Item parseInstr(size_t i, size_t j, const Function& f) {
    Item item;
    item.kind = Item::Kind::Inst;
    item.line = toks_[i].line;
    Instr& ins = item.ins;
    ins.line = item.line;
    try {
      size_t k = i;
      if (isP(k, "@")) {
        ++k;
        if (isP(k, "!")) { ins.guardNeg = true; ++k; }
        if (k >= j || !f.regs.declared(toks_[k].text)) throw ParseError("bad guard");
        ins.guard = toks_[k].text;
        ++k;
      }
      if (k >= j || toks_[k].kind != Tok::Ident) throw ParseError("expected opcode");
      auto parts = splitDots(toks_[k].text);
      ins.op = parts[0];
      ins.mods.assign(parts.begin() + 1, parts.end());
      ++k;
      while (k < j) {
        ins.ops.push_back(parseOperand(k, j, f.regs));
        if (k < j) {
          if (isP(k, ",")) ++k;
          else throw ParseError("expected ','");
        }
      }
    } catch (const ParseError&) {
      Instr raw;
      raw.raw = true;
      raw.line = item.line;
      raw.rawText = squash(src_.substr(toks_[i].begin, toks_[j - 1].end - toks_[i].begin));
      std::set<std::string> seen;
      bool opSeen = false;
      for (size_t k = i; k < j; ++k) {
        if (toks_[k].kind != Tok::Ident) continue;
        const std::string& s = toks_[k].text;
        if (s[0] == '%' && !isKnownSpecial(s)) {
          if (seen.insert(s).second) raw.rawRegs.push_back(s);
        } else if (!opSeen && s[0] != '%' && s[0] != '$') {
          auto parts = splitDots(s);
          raw.op = parts[0];
          raw.mods.assign(parts.begin() + 1, parts.end());
          opSeen = true;
        }
      }
      ins = raw;
    }
    return item;
  }

  void parseBody(size_t i0, size_t i1, Function& f) {
    size_t i = i0;
    while (i < i1) {
      const Token& t = toks_[i];
      if (t.kind == Tok::Ident && i + 1 < i1 && isP(i + 1, ":") && t.text[0] != '.') {
        Item it;
        it.kind = Item::Kind::Label;
        it.text = t.text;
        it.line = t.line;
        f.items.push_back(it);
        i += 2;
        continue;
      }
      if (t.text == ".loc" || t.text == ".file") {  // newline terminated
        size_t j = i + 1;
        while (j < i1 && toks_[j].line == t.line) ++j;
        Item it;
        it.kind = Item::Kind::Raw;
        it.text = squash(slice(i, j - 1));
        it.line = t.line;
        f.items.push_back(it);
        i = j;
        continue;
      }
      size_t j = i;
      while (j < i1 && !isP(j, ";")) ++j;
      if (j >= i1) throw ParseError("missing ';' near line " + std::to_string(t.line));
      if (t.text == ".reg") {
        parseRegDecl(i + 1, j, f);
        f.decls.push_back(squash(slice(i, j)));
      } else if (t.text[0] == '.') {
        Item it;
        it.kind = Item::Kind::Raw;
        it.text = squash(slice(i, j));
        it.line = t.line;
        f.items.push_back(it);
      } else {
        f.items.push_back(parseInstr(i, j, f));
      }
      i = j + 1;
    }
  }
};

}  // namespace

Module parseModule(const std::string& src) {
  Parser p(src);
  return p.run();
}

Module parseFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open file: " + path);
  std::stringstream ss;
  ss << in.rdbuf();
  return parseModule(ss.str());
}

}  // namespace ptx
