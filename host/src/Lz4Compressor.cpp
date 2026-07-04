// ─── Lz4Compressor.cpp ───────────────────────────────────────
// LZ4_compress_fast 的轻量封装。
// 所有繁重工作（缓冲区分配）在 Initialize() 中完成。
// Compress() 是热点路径调用，零分配。

#include "Lz4Compressor.h"
#include <lz4.h>

namespace SynapseX {

bool Lz4Compressor::Initialize(int maxInputSize) {
    if (maxInputSize <= 0) {
        return false;
    }
    m_maxInputSize = maxInputSize;
    int bound = LZ4_compressBound(maxInputSize);
    if (bound <= 0) {
        return false;
    }
    m_compressBuf.resize(static_cast<size_t>(bound));
    return true;
}

bool Lz4Compressor::Compress(const uint8_t* input, int inputSize,
                             std::vector<uint8_t>& output) {
    // 如果当前输入超过预分配缓冲区大小，则自动调整。
    // 稳定运行时不应当发生 — 这只是一个安全网。
    if (inputSize > m_maxInputSize) {
        if (!Initialize(inputSize)) {
            return false;
        }
    }

    int srcSize = inputSize;
    int dstCap  = static_cast<int>(m_compressBuf.size());

    // LZ4_compress_fast：acceleration=5 牺牲约 5% 压缩率
    // 换取约 50% 更少的 CPU 时间。包含草地/粒子/噪点的游戏场景
    // 在 accel=1 时会导致过度哈希探查，消耗大量 CPU 预算。
    // 在 accel=5 时，即使噪点帧的压缩也保持在 0.5 毫秒以下。
    int compressedSize = LZ4_compress_fast(
        reinterpret_cast<const char*>(input),
        reinterpret_cast<char*>(m_compressBuf.data()),
        srcSize,
        dstCap,
        5  // 加速 — 实时管线中速度优先于压缩比
    );

    if (compressedSize <= 0) {
        // 压缩失败（输出缓冲区太小或内部错误）。
        return false;
    }

    // 仅将有效的压缩字节复制到调用者的输出中。
    output.assign(m_compressBuf.data(),
                  m_compressBuf.data() + compressedSize);
    return true;
}

int Lz4Compressor::GetMaxOutputSize(int inputSize) {
    return LZ4_compressBound(inputSize);
}

} // namespace SynapseX
