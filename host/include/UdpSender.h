#pragma once

// ── UDP 分片与发送模块 ──────────────────────────────────────
// 将压缩后的 LZ4 缓冲区拆分成 ≤1400 字节的块，
// 每块封装 PacketHeader 头部后通过 UDP 发送。
//
// 设计约束：
//   · 发送热路径中零堆内存分配（仅使用栈缓冲区）。
//   · sendto() 是唯一的系统调用；头部组装仅涉及 memcpy。
//   · 套接字发送缓冲区自动调整大小以提升吞吐量。

#include <cstdint>
#include <string>
#include <winsock2.h>

namespace SynapseX {

class UdpSender {
public:
    UdpSender() = default;
    ~UdpSender();

    // 不可拷贝、不可移动（持有套接字）
    UdpSender(const UdpSender&) = delete;
    UdpSender& operator=(const UdpSender&) = delete;
    UdpSender(UdpSender&&) = delete;
    UdpSender& operator=(UdpSender&&) = delete;

    // 初始化 WinSock，创建 UDP 套接字，设置目标地址。
    // targetIp：本地环回用 "127.0.0.1"，远程部署用实际 IP。
    // port：     客户端监听的 UDP 端口（默认 8888）。
    bool Initialize(const std::string& targetIp, uint16_t port = 8888);

    // 将 `compressedData`（totalSize 字节）分片并通过 UDP 发送。
    // frameId：单调递增的帧计数器，嵌入每个分片中。
    // width / height：ROI 尺寸，嵌入以供客户端设定解码器大小。
    // 返回 true 表示所有分片均发送成功。
    bool SendCompressedFrame(const uint8_t* compressedData,
                             uint32_t totalSize,
                             uint32_t frameId,
                             uint16_t width,
                             uint16_t height,
                             uint8_t  modelId);

    void Cleanup();
    bool IsInitialized() const { return m_initialized; }

private:
    SOCKET      m_socket      = ~0ull;  // INVALID_SOCKET
    sockaddr_in m_targetAddr  = {};
    bool        m_initialized = false;
    bool        m_wsaStarted  = false;
};

} // namespace SynapseX
