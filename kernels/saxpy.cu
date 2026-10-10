// grid-stride loop: shows loops, induction variables and register pressure
extern "C" __global__ void saxpy(float a, const float* x, float* y, int n) {
  for (int i = blockIdx.x * blockDim.x + threadIdx.x; i < n; i += blockDim.x * gridDim.x)
    y[i] = a * x[i] + y[i];
}
