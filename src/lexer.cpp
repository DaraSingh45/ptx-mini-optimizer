#include "ptxopt/lexer.h"

#include <cctype>

namespace ptx {

namespace {
bool idStart(char c) {
  return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$' || c == '%' || c == '.';
}
bool idCont(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' || c == '.';
}
}  // namespace

std::vector<Token> lex(const std::string& s) {
  std::vector<Token> out;
  size_t i = 0, n = s.size();
  int line = 1;
  while (i < n) {
    char c = s[i];
    if (c == '\n') { ++line; ++i; continue; }
    if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
    if (c == '/' && i + 1 < n && s[i + 1] == '/') {
      while (i < n && s[i] != '\n') ++i;
      continue;
    }
    if (c == '/' && i + 1 < n && s[i + 1] == '*') {
      i += 2;
      while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) {
        if (s[i] == '\n') ++line;
        ++i;
      }
      i += 2;
      continue;
    }
    size_t b = i;
    if (c == '"') {
      ++i;
      while (i < n && s[i] != '"') {
        if (s[i] == '\\') ++i;
        ++i;
      }
      ++i;
      if (i > n) i = n;
      out.push_back({Tok::Str, s.substr(b, i - b), line, b, i});
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c))) {
      ++i;
      bool hexish = (c == '0' && i < n && std::string("xXfFdD").find(s[i]) != std::string::npos);
      while (i < n) {
        char d = s[i];
        if (std::isalnum(static_cast<unsigned char>(d)) || d == '_' || d == '.') { ++i; continue; }
        if ((d == '+' || d == '-') && !hexish && (s[i - 1] == 'e' || s[i - 1] == 'E')) { ++i; continue; }
        break;
      }
      out.push_back({Tok::Number, s.substr(b, i - b), line, b, i});
      continue;
    }
    if (idStart(c)) {
      ++i;
      while (i < n && idCont(s[i])) ++i;
      out.push_back({Tok::Ident, s.substr(b, i - b), line, b, i});
      continue;
    }
    ++i;
    out.push_back({Tok::Punct, std::string(1, c), line, b, i});
  }
  out.push_back({Tok::End, "", line, n, n});
  return out;
}

}  // namespace ptx
