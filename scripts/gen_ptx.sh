#!/usr/bin/env bash
# Generate real PTX from the CUDA kernels with nvcc (needs the CUDA toolkit; no GPU required).
#   scripts/gen_ptx.sh [sm_arch]      default arch: sm_75
set -euo pipefail
cd "$(dirname "$0")/.."
ARCH="${1:-sm_75}"
command -v nvcc >/dev/null || { echo "nvcc not found - install the CUDA toolkit (or use Google Colab)"; exit 1; }
mkdir -p generated
for k in kernels/*.cu; do
  n=$(basename "$k" .cu)
  nvcc -ptx -O3 -arch="$ARCH" "$k" -o "generated/${n}.ptx"
  nvcc -ptx -O0 -arch="$ARCH" "$k" -o "generated/${n}_O0.ptx"
  echo "generated/${n}.ptx  generated/${n}_O0.ptx"
done
