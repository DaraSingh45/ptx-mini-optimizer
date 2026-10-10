#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ptx {

// Small dynamic bitset used by the dataflow analyses.
class BitSet {
 public:
  BitSet() = default;
  explicit BitSet(size_t n) : n_(n), w_((n + 63) / 64, 0) {}
  size_t size() const { return n_; }
  void set(size_t i) { w_[i >> 6] |= (1ULL << (i & 63)); }
  void reset(size_t i) { w_[i >> 6] &= ~(1ULL << (i & 63)); }
  bool test(size_t i) const { return (w_[i >> 6] >> (i & 63)) & 1ULL; }
  bool operator==(const BitSet& o) const { return w_ == o.w_; }
  bool operator!=(const BitSet& o) const { return w_ != o.w_; }
  void unite(const BitSet& o) {
    for (size_t i = 0; i < w_.size(); ++i) w_[i] |= o.w_[i];
  }
  void subtract(const BitSet& o) {
    for (size_t i = 0; i < w_.size(); ++i) w_[i] &= ~o.w_[i];
  }
  size_t count() const {
    size_t c = 0;
    for (size_t i = 0; i < n_; ++i) c += test(i) ? 1 : 0;
    return c;
  }
  template <class F>
  void forEach(F f) const {
    for (size_t i = 0; i < n_; ++i)
      if (test(i)) f(i);
  }

 private:
  size_t n_ = 0;
  std::vector<uint64_t> w_;
};

std::string trim(const std::string& s);
bool startsWith(const std::string& s, const std::string& prefix);
std::string squash(const std::string& s);  // collapse runs of whitespace
std::string join(const std::vector<std::string>& v, const std::string& sep);
std::vector<std::string> splitDots(const std::string& s);
std::string jsonEscape(const std::string& s);

}  // namespace ptx
