#pragma once

// ─── UdpReceiver ──────────────────────────────────────────────
// 非阻塞UDP接收 + LZ4解压流水线。
//
// 生命周期：
//   1. Initialize(port) — 绑定UDP套接字，预分配缓冲区。
//   2. TryReceive() 热循环 — 排空所有可用数据报，
//      重组乱序数据块，解压完整帧。
//   3. Cleanup() — 关闭套接字，释放WinSock。
//
// 设计约束：
//   · 非阻塞套接字，积极排空 — 在一次 TryReceive() 调用中处理所有排队的数据包。
//   · 初始预分配后，TryReceive() 中零堆分配。
//   · 动态ROI：每帧从 PacketHeader 提取宽度/高度，
//     LZ4 输出按 width×height×4 验证。
//   · 帧ID环绕安全处理（int32_t 差值）。
//
// 调用方负责以高频率调用 TryReceive()
// （理想情况下约 1 ms 或更短）。

#include "ReassemblyBuffer.h"

#include <cstdint>
#include <vector>
#include <winsock2.h>

namespace SynapseX {

class UdpReceiver {
public:
    UdpReceiver() = default;
    ~UdpReceiver();

    // 不可复制、不可移动（拥有套接字）
    UdpReceiver(const UdpReceiver&) = delete;
    UdpReceiver& operator=(const UdpReceiver&) = delete;
    UdpReceiver(UdpReceiver&&) = delete;
    UdpReceiver& operator=(UdpReceiver&&) = delete;

    // ── 生命周期 ─────────────────────────────────────────────

    // 将UDP套接字绑定到 `port`，预分配所有缓冲区。
    // 不需要ROI大小 — 解压缓冲区根据每个
    // PacketHeader 中的宽度/高度逐帧调整大小。
    // 成功返回 true。
    bool Initialize(uint16_t port = 8888);

    // 关闭套接字并释放WinSock资源。
    void Cleanup();

    bool IsInitialized() const { return m_initialized; }

    // ── 热路径：接收、重组、解压 ─────────────────────────────

    // 排空套接字缓冲区中所有排队的UDP数据报。
    // 如果至少有一个完整帧被组装并成功解压，
    // 则最新的帧通过 outFrame 返回，
    // 其 frameId 通过 outFrameId 返回，函数返回 true。
    //
    // outFrame: 成功时接收 width×height×4 BGRA 像素数据。
    // outFrameId: 成功时接收单调递增的帧计数器。
    // 使用 GetLastFrameWidth()/GetLastFrameHeight() 来解释 outFrame。
    //
    // 如果没有完整帧就绪则返回 false（正常情况 — 调用方
    // 应以高频率循环）。
    bool TryReceive(std::vector<uint8_t>& outFrame, uint32_t& outFrameId);

    // ── 上一帧尺寸（TryReceive 返回 true 后有效）────────────
    uint16_t GetLastFrameWidth()  const { return m_lastFrameWidth; }
    uint16_t GetLastFrameHeight() const { return m_lastFrameHeight; }

    // ── 统计信息 ──────────────────────────────────────────────

    uint64_t GetTotalFrames()  const { return m_totalFramesReceived; }
    uint64_t GetTotalDropped() const { return m_totalDropped; }
    uint64_t GetTotalPackets() const { return m_totalPackets; }
    uint64_t GetTotalBytes()   const { return m_totalBytes; }

private:
    // 处理单个原始UDP数据报。
    // 验证头部，处理帧转换，插入数据块。
    // 如果此数据包使一帧变为完整则返回 true。
    bool ProcessDatagram(const uint8_t* data, int len);

    // 将当前重组缓冲区解压到 outFrame 中。
    // 动态调整 outFrame 大小为 frameWidth × frameHeight × 4。
    // 如果解压成功且输出与预期大小匹配则返回 true。
    bool DecompressCurrentFrame(std::vector<uint8_t>& outFrame);

    // ── 套接字 ───────────────────────────────────────────────
    SOCKET m_socket      = INVALID_SOCKET;
    bool   m_initialized = false;
    bool   m_wsaStarted  = false;

    // ── 接收缓冲区 ─────────────────────────────────────────
    // 65536 字节：远高于最大UDP数据报（20 + 1400 = 1420 字节）。
    static constexpr int kRecvBufSize = 65536;
    alignas(64) uint8_t m_recvBuf[kRecvBufSize];

    // ── 解压输出缓冲区 ────────────────────────────────────
    // 每帧动态调整为 width×height×4。
    // 跨帧重用 — 仅在 ROI 扩大时重新分配。
    std::vector<uint8_t> m_decompressBuf;

    // ── 重组状态 ─────────────────────────────────────────────
    ReassemblyBuffer m_buffer;

    // ── 上一帧尺寸 ─────────────────────────────────────────
    uint16_t m_lastFrameWidth  = 0;
    uint16_t m_lastFrameHeight = 0;

    // ── 统计信息 ─────────────────────────────────────────────
    uint64_t m_totalFramesReceived = 0;
    uint64_t m_totalDropped        = 0;
    uint64_t m_totalPackets        = 0;
    uint64_t m_totalBytes          = 0;

    uint32_t m_lastDecodedFrameId  = 0xFFFFFFFF;
    bool     m_hasDecodedAnyFrame  = false;

    // 当前活动帧未完成时为 true。
    // 帧完成或放弃时设为 false。
    bool     m_activeFrameIncomplete = false;
};

} // namespace SynapseX
