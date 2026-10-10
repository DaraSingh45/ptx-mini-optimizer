#define TILE 16
extern "C" __global__ void matmul(const float* A, const float* B, float* C, int N) {
  __shared__ float As[TILE][TILE], Bs[TILE][TILE];
  int row = blockIdx.y * TILE + threadIdx.y, col = blockIdx.x * TILE + threadIdx.x;
  float acc = 0.f;
  for (int t = 0; t < N / TILE; ++t) {
    As[threadIdx.y][threadIdx.x] = A[row * N + t * TILE + threadIdx.x];
    Bs[threadIdx.y][threadIdx.x] = B[(t * TILE + threadIdx.y) * N + col];
    __syncthreads();
    #pragma unroll
    for (int k = 0; k < TILE; ++k) acc += As[threadIdx.y][k] * Bs[k][threadIdx.x];
    __syncthreads();
  }
  C[row * N + col] = acc;
}
