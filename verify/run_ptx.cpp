// run_ptx: load a PTX file with the CUDA *Driver API*, run one of a few known kernels on
// deterministic inputs and print a checksum of the output buffer.
// Running it on the original and on the ptx-opt output must give the same checksum.
//
//   build:  make gpu-verify            (needs the CUDA toolkit + an NVIDIA GPU)
//   usage:  build/run_ptx <file.ptx> <kernel> [n]
//   kernels: vecAdd | saxpy | redundant | divergent
#include <cuda.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#define CHECK(call)                                                                  \
  do {                                                                               \
    CUresult r_ = (call);                                                            \
    if (r_ != CUDA_SUCCESS) {                                                        \
      const char* s_ = nullptr;                                                      \
      cuGetErrorString(r_, &s_);                                                     \
      std::fprintf(stderr, "CUDA error at %s:%d: %s\n", __FILE__, __LINE__, s_ ? s_ : "?"); \
      std::exit(2);                                                                  \
    }                                                                                \
  } while (0)

static uint64_t fnv(const void* p, size_t n) {
  const unsigned char* b = static_cast<const unsigned char*>(p);
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ULL;
  return h;
}

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: %s <file.ptx> <vecAdd|saxpy|redundant|divergent> [n]\n", argv[0]); return 1; }
  std::ifstream in(argv[1]);
  if (!in) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
  std::stringstream ss;
  ss << in.rdbuf();
  std::string ptx = ss.str(), kname = argv[2];
  int n = argc > 3 ? std::atoi(argv[3]) : 1000;

  CHECK(cuInit(0));
  CUdevice dev;
  CHECK(cuDeviceGet(&dev, 0));
  CUcontext ctx;
  CHECK(cuCtxCreate(&ctx, 0, dev));
  CUmodule mod;
  CHECK(cuModuleLoadData(&mod, ptx.c_str()));  // the driver JIT-compiles PTX -> SASS here
  CUfunction fn;
  CHECK(cuModuleGetFunction(&fn, mod, kname.c_str()));

  const int threads = 128, blocks = (n + threads - 1) / threads;
  std::vector<float> hf1(n), hf2(n), hout(n, 0.f);
  std::vector<unsigned> hu1(n), huout(n, 0);
  for (int i = 0; i < n; ++i) {
    hf1[i] = static_cast<float>((i * 7) % 13) * 0.5f;
    hf2[i] = static_cast<float>((i * 3) % 11) * 0.25f;
    hu1[i] = static_cast<unsigned>(i * 2654435761u);
  }
  CUdeviceptr d1, d2, d3;
  size_t bytes = static_cast<size_t>(n) * 4;
  CHECK(cuMemAlloc(&d1, bytes));
  CHECK(cuMemAlloc(&d2, bytes));
  CHECK(cuMemAlloc(&d3, bytes));
  uint64_t checksum = 0;
  std::vector<unsigned char> result(bytes);

  if (kname == "vecAdd") {
    CHECK(cuMemcpyHtoD(d1, hf1.data(), bytes));
    CHECK(cuMemcpyHtoD(d2, hf2.data(), bytes));
    CHECK(cuMemsetD8(d3, 0, bytes));
    void* args[] = {&d1, &d2, &d3, &n};
    CHECK(cuLaunchKernel(fn, blocks, 1, 1, threads, 1, 1, 0, nullptr, args, nullptr));
    CHECK(cuCtxSynchronize());
    CHECK(cuMemcpyDtoH(result.data(), d3, bytes));
  } else if (kname == "saxpy") {
    float a = 2.5f;
    CHECK(cuMemcpyHtoD(d1, hf1.data(), bytes));
    CHECK(cuMemcpyHtoD(d2, hf2.data(), bytes));
    void* args[] = {&a, &d1, &d2, &n};
    CHECK(cuLaunchKernel(fn, blocks, 1, 1, threads, 1, 1, 0, nullptr, args, nullptr));
    CHECK(cuCtxSynchronize());
    CHECK(cuMemcpyDtoH(result.data(), d2, bytes));
  } else if (kname == "redundant") {  // (unsigned* out, const unsigned* in, int n)
    CHECK(cuMemcpyHtoD(d2, hu1.data(), bytes));
    CHECK(cuMemsetD8(d1, 0, bytes));
    void* args[] = {&d1, &d2, &n};
    CHECK(cuLaunchKernel(fn, blocks, 1, 1, threads, 1, 1, 0, nullptr, args, nullptr));
    CHECK(cuCtxSynchronize());
    CHECK(cuMemcpyDtoH(result.data(), d1, bytes));
  } else if (kname == "divergent") {  // (int* out, int n) -- one block, <=128 threads
    int m = 50;
    CHECK(cuMemsetD8(d1, 0, bytes));
    void* args[] = {&d1, &m};
    CHECK(cuLaunchKernel(fn, 1, 1, 1, 128, 1, 1, 0, nullptr, args, nullptr));
    CHECK(cuCtxSynchronize());
    CHECK(cuMemcpyDtoH(result.data(), d1, 128 * 4 < bytes ? 128 * 4 : bytes));
  } else {
    std::fprintf(stderr, "unknown kernel '%s'\n", kname.c_str());
    return 1;
  }
  checksum = fnv(result.data(), result.size());
  std::printf("%s n=%d checksum=%016llx\n", kname.c_str(), n, static_cast<unsigned long long>(checksum));
  cuMemFree(d1); cuMemFree(d2); cuMemFree(d3);
  cuModuleUnload(mod);
  cuCtxDestroy(ctx);
  return 0;
}
