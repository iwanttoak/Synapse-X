#pragma once

// ─── CudaPreprocess ───────────────────────────────────────────
// GPU端 BGRA8 → FP32 CHW RGB 预处理，通过 NVRTC 实现。
//
// 使用 NVRTC（NVIDIA 运行时编译）在初始化时编译 CUDA 内核
// — 构建时无需 nvcc。
//
// InitCudaPreprocess():
//   编译内核，加载 PTX，缓存 CUfunction 句柄。
//   必须在 cudaSetDevice() 之后调用一次，可从任何线程调用。
//
// LaunchBgra8ToFp32ChwRgb():
//   将内核排队到给定的 CUDA 流上。
//   只要流不在线程间共享，就是线程安全的。

#include <cstdint>

#ifdef SX_HAS_CUDA
#include <cuda_runtime.h>
#else
using cudaStream_t = void*;
#endif

namespace SynapseX {

// 一次性初始化。成功返回 true。
bool InitCudaPreprocess();

// 在给定的 CUDA 流上启动 BGRA→FP32 CHW RGB 内核。
//   d_bgra:    GPU 指针，指向 uint8 BGRA，大小 = width × height × 4
//   d_rgb_chw: GPU 指针，指向 float RGB CHW，大小 = 3 × width × height
//   width, height: 图像尺寸（例如 416, 416）
//   stream:    用于排队内核的 CUDA 流
void LaunchBgra8ToFp32ChwRgb(
    const uint8_t* d_bgra,
    float*         d_rgb_chw,
    int            width,
    int            height,
    cudaStream_t   stream);

} // namespace SynapseX
