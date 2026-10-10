// written to be full of redundancy so that an -O0 build gives the optimiser real work
extern "C" __global__ void redundant(unsigned* out, const unsigned* in, int n) {
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  const int four = 4, eight = 8;
  int k = four + eight;              // constant folding
  unsigned a = in[i] * 8u;           // strength reduction
  unsigned b = in[i] / 16u + in[i] % 16u;
  int unused = i * 37 + k;           // dead code
  if (i < n) out[i] = a + b + k + (i * 1) + 0;
}
