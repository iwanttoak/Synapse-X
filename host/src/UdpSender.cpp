// ─── UdpSender.cpp ───────────────────────────────────────────
// 热点路径：SendCompressedFrame() — 零堆分配。
// 每个数据块在栈上组装并通过 sendto() 发送。
//
// 单个 UDP 数据报的布局：
//   ┌──────────────┬─────────────────────────────────┐
//   │ PacketHeader │        负载（LZ4 切片）          │
//   │   24 字节    │         ≤ MAX_PAYLOAD_SIZE       │
//   └──────────────┴─────────────────────────────────┘

#include "UdpSender.h"
#include "PacketHeader.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace SynapseX {

// ── 常量 ──────────────────────────────────────────────
static constexpr int kSendBufSize = 4 * 1024 * 1024;  // 4 MB 套接字缓冲区

// ═══════════════════════════════════════════════════════════════
//  生命周期
// ═══════════════════════════════════════════════════════════════

UdpSender::~UdpSender() {
    Cleanup();
}

bool UdpSender::Initialize(const std::string& targetIp, uint16_t port) {
    if (m_initialized) Cleanup();

    // ── WinSock 启动 ──────────────────────────────────
    WSADATA wsaData = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "[UdpSender] WSAStartup 失败：%d\n", WSAGetLastError());
        return false;
    }
    m_wsaStarted = true;

    // ── 创建 UDP 套接字 ────────────────────────────────
    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == INVALID_SOCKET) {
        fprintf(stderr, "[UdpSender] socket() 失败：%d\n", WSAGetLastError());
        Cleanup();
        return false;
    }

    // ── 非阻塞模式 — 防止 sendto() 在网络堆栈拥塞时
    //    阻塞主循环。
    u_long nonBlocking = 1;
    ioctlsocket(m_socket, FIONBIO, &nonBlocking);

    // ── 增大发送缓冲区至 4 MB ──────────────────────
    // 在 170 Hz 下，每帧约 50 KB，缓冲区可以在阻塞前
    // 吸收约 80 帧的突发数据。
    int bufSize = kSendBufSize;
    setsockopt(m_socket, SOL_SOCKET, SO_SNDBUF,
               reinterpret_cast<const char*>(&bufSize), sizeof(bufSize));

    // ── 解析目标地址 ───────────────────────────
    std::memset(&m_targetAddr, 0, sizeof(m_targetAddr));
    m_targetAddr.sin_family = AF_INET;
    m_targetAddr.sin_port   = htons(port);

    if (inet_pton(AF_INET, targetIp.c_str(), &m_targetAddr.sin_addr) != 1) {
        fprintf(stderr, "[UdpSender] inet_pton 失败，目标 '%s'：%d\n",
                targetIp.c_str(), WSAGetLastError());
        Cleanup();
        return false;
    }

    m_initialized = true;
    fprintf(stderr, "[UdpSender] 就绪 -- 目标 %s:%u, 发送缓冲区 %d KB, 非阻塞\n",
            targetIp.c_str(), port, kSendBufSize / 1024);
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  发送（热点路径）
// ═══════════════════════════════════════════════════════════════

bool UdpSender::SendCompressedFrame(const uint8_t* compressedData,
                                     uint32_t totalSize,
                                     uint32_t frameId,
                                     uint16_t width,
                                     uint16_t height,
                                     uint8_t  modelId) {
    if (!m_initialized) return false;
    if (totalSize == 0)   return false;

    // ── 计算块数 ────────────────────────────
    const uint16_t totalChunks = static_cast<uint16_t>(
        (totalSize + MAX_PAYLOAD_SIZE - 1) / MAX_PAYLOAD_SIZE);

    // ── 栈分配的数据包缓冲区 ────────────────────
    //    发送循环中零堆分配。
    constexpr size_t kPacketBufSize = sizeof(PacketHeader) + MAX_PAYLOAD_SIZE;
    uint8_t packetBuf[kPacketBufSize];

    // 预填充每帧不变的头部字段。
    auto* header = reinterpret_cast<PacketHeader*>(packetBuf);
    header->magic       = PROTOCOL_MAGIC;
    header->frameId     = frameId;
    header->totalChunks = totalChunks;
    header->totalSize   = totalSize;
    header->width       = width;
    header->height      = height;
    header->modelId     = modelId;

    const uint8_t* src = compressedData;
    uint32_t remaining = totalSize;

    for (uint16_t i = 0; i < totalChunks; ++i) {
        // 每个块的头部字段
        header->chunkIndex  = i;
        header->payloadSize = (remaining > MAX_PAYLOAD_SIZE)
                              ? MAX_PAYLOAD_SIZE
                              : static_cast<uint16_t>(remaining);

        // 将负载复制到数据包缓冲区（头部之后）
        std::memcpy(packetBuf + sizeof(PacketHeader),
                    src,
                    header->payloadSize);

        // 发送
        int totalPacketSize = static_cast<int>(sizeof(PacketHeader) + header->payloadSize);
        int sent = sendto(m_socket,
                          reinterpret_cast<const char*>(packetBuf),
                          totalPacketSize,
                          0,
                          reinterpret_cast<const sockaddr*>(&m_targetAddr),
                          sizeof(m_targetAddr));

        if (sent == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                // 非阻塞发送缓冲区已满 — 丢弃此块。
                // 客户端会重新组装乱序数据；丢失一个块
                // 意味着该帧丢失，但管线继续运行。
                // 在 170 Hz 下，单帧丢失不可见。
                static int dropCount = 0;
                if (++dropCount % 100 == 1) {
                    fprintf(stderr, "[UdpSender] 数据包已丢弃（发送缓冲区满，"
                            "共 %d 次丢弃）\n", dropCount);
                }
                return false;
            }
            fprintf(stderr, "[UdpSender] sendto 失败，块 %u/%u：错误码=%d\n",
                    i + 1, totalChunks, err);
            return false;
        }

        src       += header->payloadSize;
        remaining -= header->payloadSize;
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════
//  清理
// ═══════════════════════════════════════════════════════════════

void UdpSender::Cleanup() {
    if (m_socket != INVALID_SOCKET) {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }
    if (m_wsaStarted) {
        WSACleanup();
        m_wsaStarted = false;
    }
    m_initialized = false;
}

} // namespace SynapseX
