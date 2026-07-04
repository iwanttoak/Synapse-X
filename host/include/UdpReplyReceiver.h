#pragma once

// ── UDP 回复接收器 ─────────────────────────────────────────
// 监听 UDP 端口（默认 8889），接收来自客户端的推理回复。
// 每条回复携带模型像素空间中的检测边界框；
// 本模块利用主机已知的 ROI 偏移量将其映射到屏幕坐标，
// 以供叠加层渲染使用。
//
// 用法：
//   UdpReplyReceiver receiver;
//   receiver.Initialize(8889);
//   receiver.SetRoiParams(roiW, roiH, screenW, screenH);
//   // 在主循环中：
//   receiver.ReceiveReplies(detections, latestFrameId);

#include <cstdint>
#include <vector>
#include <winsock2.h>

namespace SynapseX {

// ── 屏幕空间映射后的检测结果（可供叠加层直接使用）───────────
struct Detection {
    float x1, y1, x2, y2;     // 屏幕空间坐标
    float confidence;
    uint32_t classId;          // 0 = 敌人，1 = 队友
};

class UdpReplyReceiver {
public:
    UdpReplyReceiver() = default;
    ~UdpReplyReceiver();

    UdpReplyReceiver(const UdpReplyReceiver&) = delete;
    UdpReplyReceiver& operator=(const UdpReplyReceiver&) = delete;

    // 将 UDP 套接字绑定到 `port`（默认 8889）。
    bool Initialize(uint16_t port = 8889);
    void Cleanup();
    bool IsInitialized() const { return m_initialized; }

    // 设置 ROI 和屏幕几何参数，用于坐标映射。
    // 必须在调用 ReceiveReplies 之前调用。
    void SetRoiParams(int roiW, int roiH, int screenW, int screenH);

    // 从套接字缓冲区中取出所有已排队的回复数据报。
    // 返回已映射到屏幕坐标的检测结果。
    // outDetections：追加解码后的检测结果（不会预先清空）。
    // outLatestFrameId：设置为最新回复的 frameId。
    // 返回 true 表示至少收到一条有效回复。
    bool ReceiveReplies(std::vector<Detection>& outDetections,
                        uint32_t& outLatestFrameId);

private:
    SOCKET m_socket      = INVALID_SOCKET;
    bool   m_initialized = false;
    bool   m_wsaStarted  = false;

    int m_roiW = 640, m_roiH = 640;
    int m_screenW = 1920, m_screenH = 1080;
    int m_roiX = 0, m_roiY = 0;  // 计算得出的中心裁剪偏移量

    // 最大可能回复：ReplyHeader(16) + 50*DetectionRaw(24) = 1216 字节
    static constexpr int kRecvBufSize = 2048;
    alignas(64) uint8_t m_recvBuf[kRecvBufSize];

    void RecalculateRoiOffset();
};

} // namespace SynapseX
