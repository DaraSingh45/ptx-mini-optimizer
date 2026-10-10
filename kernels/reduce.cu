// shared-memory tree reduction: barriers + loop (analysis only; the built-in interpreter skips bar.sync)
extern "C" __global__ void reduce(const float* in, float* out) {
  extern __shared__ float sdata[];
  unsigned t = threadIdx.x;
  sdata[t] = in[blockIdx.x * blockDim.x + t];
  __syncthreads();
  for (unsigned s = blockDim.x / 2; s > 0; s >>= 1) {
    if (t < s) sdata[t] += sdata[t + s];
    __syncthreads();
  }
  if (t == 0) out[blockIdx.x] = sdata[0];
}
