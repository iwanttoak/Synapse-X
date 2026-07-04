#pragma once

// ─── ReassemblyBuffer ─────────────────────────────────────────
// 乱序UDP数据块重组引擎。
//
// 严格遵循 HOST_SPEC.md 第3节算法：
//   · 数据块放置在偏移量 = chunkIndex * MAX_PAYLOAD_SIZE 处
//   · 重复数据块（通过 receivedMask 检测）被静默丢弃
//   · 更新的 frameId → 旧的未完整帧立即丢弃
//     （低延迟视觉的铁律——绝不等待掉队者）
//   · 过期数据包（frameId 落后于 expectedFrameId）被丢弃
//
// 所有缓冲区预分配到最坏情况大小（4096×4096 ROI），
// 以避免接收热路径中的堆分配。在稳态下，
// 仅当压缩帧超过当前容量时 StartFrame() 才会重新分配数据，
// 而对于任何实际的 ROI，最坏情况预分配使这种情况永远不会发生。
//
// 支持动态ROI：frameWidth / frameHeight 从 PacketHeader 传递到解压验证。

#include "PacketHeader.h"

#include <lz4.h>
#include <algorithm>

#include <cstdint>
#include <cstring>
#include <vector>

namespace SynapseX {

// ── 最坏情况预分配常量（4096 × 4096 ROI）─────────────────────
// 主机可以发送从 416² 到 4096² 的任何 ROI。我们按最坏情况预分配，
// 以保证每帧零堆分配。
constexpr uint32_t kMaxRoiPixels       = 4096 * 4096;            // 16,777,216 px
constexpr uint32_t kMaxRawFrameSize    = kMaxRoiPixels * 4;      // 67,108,864 bytes
constexpr uint32_t kMaxCompressedSize  = 67372052;               // LZ4_compressBound(67108864)
constexpr uint16_t kMaxChunks          = 48123;                  // ceil(67372052 / 1400)

// ── 帧ID环绕安全比较 ────────────────────────────────────────
// 如果 `a` 比 `b` 更新则返回 true，正确处理
// uint32_t 环绕（在 170 Hz 下约 290 天后环绕）。
inline bool IsNewerFrameId(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) > 0;
}

// ═══════════════════════════════════════════════════════════════
//  ReassemblyBuffer 重组缓冲区
// ═══════════════════════════════════════════════════════════════
struct ReassemblyBuffer {
    // ── 当前帧状态 ──────────────────────────────────────────
    uint32_t expectedFrameId = 0xFFFFFFFF;  // 哨兵值：无活动帧
    uint32_t totalSize       = 0;           // 活动帧的压缩大小
    uint16_t totalChunks     = 0;           // 预期的数据块总数
    uint16_t chunksReceived  = 0;           // 已收集的唯一数据块数
    uint16_t frameWidth      = 0;           // ROI宽度（来自 PacketHeader）
    uint16_t frameHeight     = 0;           // ROI高度（来自 PacketHeader）

    // ── 便捷方法 ─────────────────────────────────────────────
    uint32_t GetRawFrameSize() const {
        return static_cast<uint32_t>(frameWidth) *
               static_cast<uint32_t>(frameHeight) * 4;
    }

    // ── 预分配存储 ──────────────────────────────────────────
    // receivedMask: 跟踪哪些数据块已到达的位掩码。
    // data:         压缩负载的重组缓冲区。
    // 两者都预分配到最坏情况大小（4096² ROI）。
    std::vector<bool>    receivedMask;
    std::vector<uint8_t> data;

    // ── 构造函数：预分配到最坏情况 ──────────────────────────
    ReassemblyBuffer() {
        receivedMask.reserve(kMaxChunks);
        data.reserve(kMaxCompressedSize);
    }

    // ── StartFrame ───────────────────────────────────────────
    // 开始收集新帧。任何旧的未完成帧都会被
    // 不可撤销地丢弃（如果未完成，调用方必须已将其计为丢弃）。
    //
    // width/height 从 PacketHeader 中提取并传递到解压验证。
    void StartFrame(uint32_t frameId,
                    uint32_t totalSz,
                    uint16_t totalCh,
                    uint16_t width,
                    uint16_t height) {
        expectedFrameId = frameId;
        totalSize       = totalSz;
        totalChunks     = totalCh;
        chunksReceived  = 0;
        frameWidth      = width;
        frameHeight     = height;

        // 仅当此帧的压缩负载超过预分配容量时才扩大数据缓冲区。
        // 使用 64 MiB 最坏情况预分配，实践中永远不会触发此操作。
        if (data.size() < totalSz) {
            data.resize(totalSz);
        }

        receivedMask.assign(totalCh, false);
    }

    // ── InsertChunk ──────────────────────────────────────────
    // 将 `payload`（payloadSize 字节）复制到重组缓冲区中
    // 偏移量 = chunkIndex * MAX_PAYLOAD_SIZE 处。
    // 如果数据块是新的（非重复）则返回 true。
    // 对于重复数据块或超出范围索引返回 false。
    inline bool InsertChunk(uint16_t chunkIndex,
                            const uint8_t* payload,
                            uint16_t payloadSize) {
        if (chunkIndex >= totalChunks) return false;
        if (receivedMask[chunkIndex]) return false;  // 重复 — 丢弃

        const uint32_t offset = static_cast<uint32_t>(chunkIndex) * MAX_PAYLOAD_SIZE;
        std::memcpy(data.data() + offset, payload, payloadSize);

        receivedMask[chunkIndex] = true;
        ++chunksReceived;
        return true;
    }

    // ── IsComplete ───────────────────────────────────────────
    inline bool IsComplete() const {
        return totalChunks > 0 && chunksReceived == totalChunks;
    }

    // ── HasActiveFrame ───────────────────────────────────────
    inline bool HasActiveFrame() const {
        return expectedFrameId != 0xFFFFFFFF;
    }

    // ── Reset ────────────────────────────────────────────────
    // 完全丢弃所有状态。在清理或完成帧被消费后使用。
    void Reset() {
        expectedFrameId = 0xFFFFFFFF;
        totalSize       = 0;
        totalChunks     = 0;
        chunksReceived  = 0;
        frameWidth      = 0;
        frameHeight     = 0;
        receivedMask.clear();
        // data 有意保留（重用容量）
    }
};

} // namespace SynapseX
