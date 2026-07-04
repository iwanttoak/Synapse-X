#pragma once

// ── LZ4 高速压缩模块 ───────────────────────────────────────
// 封装 LZ4_compress_fast，使用预分配缓冲区以避免热路径中
// 的重复内存分配。
//
// 用法：
//   Lz4Compressor comp;
//   comp.Initialize(640 * 640 * 4);   // 为 BGRA ROI 预分配空间
//   comp.Compress(input, size, output);

#include <cstdint>
#include <vector>

namespace SynapseX {

class Lz4Compressor {
public:
    Lz4Compressor() = default;

    // 为给定最大输入尺寸预分配内部压缩缓冲区。
    // 初始化阶段调用一次，无需每帧调用。
    bool Initialize(int maxInputSize);

    // 将 `input`（inputSize 字节）压缩到 `output` 中。
    // output 会自动调整大小以精确容纳压缩后的数据。
    // 成功返回 true，压缩失败返回 false。
    bool Compress(const uint8_t* input, int inputSize,
                  std::vector<uint8_t>& output);

    // 压缩数据大小的最坏情况上限（LZ4_compressBound）。
    static int GetMaxOutputSize(int inputSize);

    int GetMaxInputSize() const { return m_maxInputSize; }

private:
    int m_maxInputSize = 0;
    std::vector<uint8_t> m_compressBuf; // 预分配的最坏情况缓冲区
};

} // namespace SynapseX
