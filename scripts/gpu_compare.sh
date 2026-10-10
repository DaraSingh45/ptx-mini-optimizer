#!/usr/bin/env bash
# Real-GPU check: run original and optimised PTX through the CUDA driver (JIT) and compare checksums.
#   scripts/gpu_compare.sh <original.ptx> <kernel: vecAdd|saxpy|redundant|divergent>
set -euo pipefail
cd "$(dirname "$0")/.."
[ $# -eq 2 ] || { echo "usage: $0 <original.ptx> <kernel>"; exit 1; }
export CUDA_HOME="${CUDA_HOME:-/usr/local/cuda}"
make -s build/ptx-opt gpu-verify
mkdir -p out
./build/ptx-opt optimize "$1" -o out/gpu_opt.ptx
a=$(./build/run_ptx "$1" "$2")
b=$(./build/run_ptx out/gpu_opt.ptx "$2")
echo "original : $a"; echo "optimised: $b"
[ "${a#* }" = "${b#* }" ] && echo "RESULT: identical output on the real GPU" || { echo "RESULT: MISMATCH"; exit 2; }
