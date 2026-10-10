#!/usr/bin/env bash
# One-shot demo: build, test, analyse, optimise, draw CFGs.   usage: scripts/demo.sh
set -euo pipefail
cd "$(dirname "$0")/.."
make -j"$(nproc 2>/dev/null || echo 2)"
make test
mkdir -p out
for f in samples/*.ptx; do
  n=$(basename "$f" .ptx)
  echo; echo "################ $n"
  ./build/ptx-opt analyze "$f"
  ./build/ptx-opt optimize "$f" -o "out/${n}.opt.ptx" --stats --verify || true
done
if command -v dot >/dev/null; then
  for f in samples/*.ptx; do ./build/ptx-opt cfg "$f" --out out/cfg --dom >/dev/null; done
  for d in out/cfg/*.dot; do dot -Tpng "$d" -o "${d%.dot}.png"; done
  echo; echo "CFG images: out/cfg/*.png"
else
  echo "(graphviz 'dot' not installed - skipping CFG images)"
fi
echo; echo "optimised PTX files: out/*.opt.ptx"
