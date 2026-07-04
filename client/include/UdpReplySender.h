#pragma once

// ─── UdpReplySender ──────────────────────────────────────────
// 将推理检测结果通过UDP发送回主机。
//
// 生命周期：
//   1. Initialize(hostIp, port) — 创建UDP套接字，设置目标。
//   2. SendReplies(frameId, dets) — 打包并发送单个数据报。
//   3. Cleanup() — 关闭套接字。
//
// 回复数据报格式：ReplyHeader (16B) + DetectionRaw[] (N×24B)
// 所有字段均为小端序（原生 x64）。

#include <cstdint>
#include <string>
#include <vector>
#include <winsock2.h>

namespace SynapseX {

struct Detection;  // 从 TrtInference.h 前向声明

class UdpReplySender {
public:
    UdpReplySender() = default;
    ~UdpReplySender();

    UdpReplySender(const UdpReplySender&) = delete;
    UdpReplySender& operator=(const UdpReplySender&) = delete;
    UdpReplySender(UdpReplySender&&) = delete;
    UdpReplySender& operator=(UdpReplySender&&) = delete;

    // 连接到主机的回复端口（通常为 8889）。
    bool Initialize(const std::string& hostIp, uint16_t port = 8889);

    // 将单帧的检测结果发送给主机。
    // dets: 推理输出（最多50个，超出则静默截断）。
    bool SendReplies(uint32_t frameId, const std::vector<Detection>& dets);

    void Cleanup();
    bool IsInitialized() const { return m_initialized; }

private:
    SOCKET      m_socket      = INVALID_SOCKET;
    sockaddr_in m_targetAddr  = {};
    bool        m_initialized = false;
    bool        m_wsaStarted  = false;
};

} // namespace SynapseX
