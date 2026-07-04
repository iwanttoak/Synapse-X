// ─── CudaPreprocess.cpp ────────────────────────────────────────
// GPU BGRA8 → FP32 CHW RGB 预处理，通过 NVRTC 运行时编译实现。
//
// 完全避免构建时对 nvcc 的依赖。CUDA 内核以字符串形式存储，
// 在初始化时由 NVRTC 编译。编译后的 PTX 在处理过程的生命周期内缓存。

#include "CudaPreprocess.h"
#include "Log.h"

#include <cuda_runtime.h>
#include <cuda.h>
#include <nvrtc.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace SynapseX {

// ═══════════════════════════════════════════════════════════════
//  CUDA 内核源码（嵌入式字符串）
// ═══════════════════════════════════════════════════════════════

static const char* kKernelSource = R"(
extern "C" __global__ void Bgra8ToFp32ChwRgbKernel(
    const unsigned char* __restrict__ d_bgra,
    float* __restrict__ d_rgb_chw,
    int width,
    int height)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    int srcIdx    = (y * width + x) * 4;
    int planeSize = width * height;
    int dstIdx    = y * width + x;

    unsigned char b = d_bgra[srcIdx + 0];
    unsigned char g = d_bgra[srcIdx + 1];
    unsigned char r = d_bgra[srcIdx + 2];

    d_rgb_chw[0 * planeSize + dstIdx] = __fdividef((float)r, 255.0f);
    d_rgb_chw[1 * planeSize + dstIdx] = __fdividef((float)g, 255.0f);
    d_rgb_chw[2 * planeSize + dstIdx] = __fdividef((float)b, 255.0f);
}
)";

// ═══════════════════════════════════════════════════════════════
//  缓存状态（一次性初始化）
// ═══════════════════════════════════════════════════════════════

static bool        s_initialized = false;
static CUmodule    s_module      = nullptr;
static CUfunction  s_kernel      = nullptr;
static std::string s_buildLog;

static void CheckCudaError(CUresult err, const char* tag) {
    if (err != CUDA_SUCCESS) {
        const char* name = nullptr;
        cuGetErrorName(err, &name);
        const char* str = nullptr;
        cuGetErrorString(err, &str);
        SX_LOG_ERROR("[CudaPreprocess] {} failed: {} ({})",
                     tag, name ? name : "?", str ? str : "?");
    }
}

// ═══════════════════════════════════════════════════════════════
//  InitCudaPreprocess（初始化CUDA预处理）
// ═══════════════════════════════════════════════════════════════

bool InitCudaPreprocess() {
    if (s_initialized) return true;

    // ── 1. 创建 NVRTC 程序 ─────────────────────────────────
    nvrtcProgram prog = nullptr;
    nvrtcResult nvr = nvrtcCreateProgram(&prog, kKernelSource,
        "Bgra8ToFp32ChwRgbKernel", 0, nullptr, nullptr);
    if (nvr != NVRTC_SUCCESS) {
        SX_LOG_ERROR("[CudaPreprocess] nvrtcCreateProgram failed: {}",
                     nvrtcGetErrorString(nvr));
        return false;
    }

    // ── 2. 编译 ────────────────────────────────────────────
    // 使用 compute_86 以获得广泛的 GPU 支持（PTX JIT 处理更新的架构）
    const char* opts[] = {
        "--gpu-architecture=compute_86",
        "-std=c++17",
        "-use_fast_math",
        "-restrict"
    };
    nvr = nvrtcCompileProgram(prog, 4, opts);

    // ── 3. 获取编译日志（总是获取，用于诊断）────────────
    size_t logSize = 0;
    nvrtcGetProgramLogSize(prog, &logSize);
    if (logSize > 1) {
        s_buildLog.resize(logSize);
        nvrtcGetProgramLog(prog, &s_buildLog[0]);
        SX_LOG_DEBUG("[CudaPreprocess] NVRTC build log:\n{}", s_buildLog);
    }

    if (nvr != NVRTC_SUCCESS) {
        SX_LOG_ERROR("[CudaPreprocess] nvrtcCompileProgram failed: {}",
                     nvrtcGetErrorString(nvr));
        nvrtcDestroyProgram(&prog);
        return false;
    }

    // ── 4. 获取编译后的 PTX ─────────────────────────────────
    size_t ptxSize = 0;
    nvrtcGetPTXSize(prog, &ptxSize);
    std::vector<char> ptx(ptxSize);
    nvrtcGetPTX(prog, ptx.data());
    nvrtcDestroyProgram(&prog);

    SX_LOG_INFO("[CudaPreprocess] Kernel compiled via NVRTC: ptx_bytes={}", ptxSize);

    // ── 5. 将 PTX 加载到 CUDA 模块中 ──────────────────────
    CUresult cu = cuModuleLoadData(&s_module, ptx.data());
    if (cu != CUDA_SUCCESS) {
        CheckCudaError(cu, "cuModuleLoadData");
        return false;
    }

    // ── 6. 获取内核函数句柄 ───────────────────────────────
    cu = cuModuleGetFunction(&s_kernel, s_module,
                             "Bgra8ToFp32ChwRgbKernel");
    if (cu != CUDA_SUCCESS) {
        CheckCudaError(cu, "cuModuleGetFunction");
        cuModuleUnload(s_module);
        s_module = nullptr;
        return false;
    }

    s_initialized = true;
    SX_LOG_INFO("[CudaPreprocess] Ready: runtime NVRTC kernel available");
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  LaunchBgra8ToFp32ChwRgb（启动内核）
// ═══════════════════════════════════════════════════════════════

void LaunchBgra8ToFp32ChwRgb(
    const uint8_t* d_bgra,
    float*         d_rgb_chw,
    int            width,
    int            height,
    cudaStream_t   stream)
{
    if (!s_initialized) {
        SX_LOG_ERROR("[CudaPreprocess] Not initialized; call InitCudaPreprocess() first");
        return;
    }

    // 内核启动配置
    const dim3 block(16, 16);
    const dim3 grid(
        (static_cast<unsigned int>(width)  + block.x - 1) / block.x,
        (static_cast<unsigned int>(height) + block.y - 1) / block.y);

    // 参数：d_bgra, d_rgb_chw, width, height
    void* args[] = {
        const_cast<uint8_t**>(&d_bgra),
        &d_rgb_chw,
        const_cast<int*>(&width),
        const_cast<int*>(&height)
    };

    CUresult cu = cuLaunchKernel(
        s_kernel,
        grid.x,  grid.y,  1,       // 网格
        block.x, block.y, 1,       // 块
        0,                          // 共享内存
        stream,                     // CUDA 流
        args,                       // 内核参数
        nullptr                     // 额外
    );
#ifdef _DEBUG
    CheckCudaError(cu, "cuLaunchKernel");
#else
    (void)cu;
#endif
}

} // namespace SynapseX
