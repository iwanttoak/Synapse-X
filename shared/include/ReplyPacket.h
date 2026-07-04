#pragma once

// ── 客户端 → 主机回复协议 ──────────────────────────────────
// 推理完成后，客户端通过 UDP 将检测结果发送回主机。
// 每条回复均适合单个数据报传输。
//
// 布局：
//   ┌──────────────┬─────────────────────────────────┐
//   │ ReplyHeader  │     DetectionRaw[]              │
//   │   16 字节     │   numDets × 24 字节             │
//   └──────────────┴─────────────────────────────────┘

#include <cstdint>

namespace SynapseX {

constexpr uint16_t REPLY_MAGIC = 0x5359;  // 'SY' — 与数据魔数 (0x5358) 区分
constexpr uint16_t MAX_DETS_PER_REPLY = 50;  // 50 × 24 + 16 = 1216 字节，适合 MTU

#pragma pack(push, 1)
struct ReplyHeader {
    uint16_t magic   = REPLY_MAGIC;
    uint32_t frameId = 0;          // 与主机的 frameId 对应
    uint16_t numDets = 0;          // DetectionRaw 条目数量
    uint8_t  padding[8] = {};      // 保留
};
#pragma pack(pop)
static_assert(sizeof(ReplyHeader) == 16, "ReplyHeader 必须为 16 字节");

#pragma pack(push, 1)
struct DetectionRaw {
    float    x1, y1;               // 左上角，模型像素坐标
    float    x2, y2;               // 右下角
    float    confidence;
    uint32_t classId;              // 0 = 敌人，1 = 队友
};
#pragma pack(pop)
static_assert(sizeof(DetectionRaw) == 24, "DetectionRaw 必须为 24 字节");

} // namespace SynapseX
