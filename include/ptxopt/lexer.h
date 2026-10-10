#pragma once
#include <string>
#include <vector>

namespace ptx {

enum class Tok { Ident, Number, Str, Punct, End };

struct Token {
  Tok kind = Tok::End;
  std::string text;
  int line = 0;
  size_t begin = 0, end = 0;  // byte offsets in the source
};

// Tokenises PTX source. Comments are dropped. The last token is always Tok::End.
std::vector<Token> lex(const std::string& src);

}  // namespace ptx
