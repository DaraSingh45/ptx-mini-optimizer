#include "ptxopt/util.h"

#include <cctype>

namespace ptx {

std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

bool startsWith(const std::string& s, const std::string& p) {
  return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

std::string squash(const std::string& s) {
  std::string out;
  bool inStr = false, pendingSpace = false;
  for (char c : s) {
    if (c == '"') inStr = !inStr;
    if (!inStr && std::isspace(static_cast<unsigned char>(c))) {
      pendingSpace = !out.empty();
      continue;
    }
    if (pendingSpace) out += ' ';
    pendingSpace = false;
    out += c;
  }
  return out;
}

std::string join(const std::vector<std::string>& v, const std::string& sep) {
  std::string r;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) r += sep;
    r += v[i];
  }
  return r;
}

std::vector<std::string> splitDots(const std::string& s) {
  std::vector<std::string> r;
  std::string cur;
  for (char c : s) {
    if (c == '.') {
      r.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  r.push_back(cur);
  return r;
}

std::string jsonEscape(const std::string& s) {
  std::string o;
  for (char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\t': o += "\\t"; break;
      default: o += c;
    }
  }
  return o;
}

}  // namespace ptx
