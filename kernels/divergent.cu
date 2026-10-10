// one thread-divergent branch (depends on threadIdx) and one uniform branch (depends on a parameter)
extern "C" __global__ void divergent(int* out, int n) {
  int t = threadIdx.x;
  int v;
  if (t & 1) v = t * 3; else v = t << 2;
  if (n > 40) v += 200; else v += 100;
  out[t] = v;
}
