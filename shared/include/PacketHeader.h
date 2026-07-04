#pragma once
#include <cstdint>

// ── 应用层分片协议 ─────────────────────────────────────────
// 每个压缩帧被拆分为 N 个适合 UDP 传输的分片。
// 接收方根据相同的 FrameID 重新组装分片。
//
//         ┌─────────────────────────┬────────────────────────┐
//         │     PacketHeader        │   载荷（LZ4 数据）     │
//         │      24 字节            │     ≤ MAX_PAYLOAD      │
//         └─────────────────────────┴────────────────────────┘

namespace SynapseX {

constexpr uint16_t MAX_PAYLOAD_SIZE = 1400;
constexpr uint16_t PROTOCOL_MAGIC   = 0x5358;  // 'SX'

#pragma pack(push, 1)
struct PacketHeader {
    uint16_t magic        = PROTOCOL_MAGIC;  // 0: 协议魔数
    uint32_t frameId      = 0;               // 2: 单调递增帧计数器
    uint16_t totalChunks  = 0;               // 6: 本帧分片总数
    uint16_t chunkIndex   = 0;               // 8: 基于 0 的分片索引
    uint32_t totalSize    = 0;               // 10: 压缩数据总大小（字节）
    uint16_t payloadSize  = 0;               // 14: 本数据包载荷字节数
    uint16_t width        = 0;               // 16: ROI 宽度
    uint16_t height       = 0;               // 18: ROI 高度
    uint8_t  modelId      = 0;               // 20: 目标模型（对应引擎文件）
    uint8_t  padding[3]   = {0};             // 21-23: 保留
};
#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 24, "PacketHeader 必须为 24 字节");

constexpr uint16_t MAX_CHUNKS_PER_FRAME = 65535;

} // namespace SynapseX

// ── 模型切换信号 ──────────────────────────────────────────
// 由 UdpReceiver 在收到有效新帧时写入。
// 由 TrtInference 在每次 Infer() 调用开始时读取。
#include <atomic>
extern std::atomic<uint8_t> g_targetModelId;
