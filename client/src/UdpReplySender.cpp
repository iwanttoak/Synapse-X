// ─── UdpReplySender.cpp ────────────────────────────────────────
// 将检测结果打包为紧凑的UDP回复并发送给主机。

#include "UdpReplySender.h"
#include "Log.h"
#include "TrtInference.h"
#include "ReplyPacket.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

namespace SynapseX {

// ── 常量 ──────────────────────────────────────────────────
static constexpr int kSendBufSize = 64 * 1024;  // 64 KB 套接字发送缓冲区

// ═══════════════════════════════════════════════════════════════
//  生命周期
// ═══════════════════════════════════════════════════════════════

UdpReplySender::~UdpReplySender() {
    Cleanup();
}

bool UdpReplySender::Initialize(const std::string& hostIp, uint16_t port) {
    if (m_initialized) Cleanup();

    // ── WinSock 启动 ────────────────────────────────────
    WSADATA wsaData = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        SX_LOG_ERROR("[UdpReplySender] WSAStartup 失败: {}", WSAGetLastError());
        return false;
    }
    m_wsaStarted = true;

    // ── 创建UDP套接字 ──────────────────────────────────────
    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == INVALID_SOCKET) {
        SX_LOG_ERROR("[UdpReplySender] socket() 失败: {}", WSAGetLastError());
        Cleanup();
        return false;
    }

    // ── 扩大发送缓冲区 ─────────────────────────────────
    int bufSize = kSendBufSize;
    setsockopt(m_socket, SOL_SOCKET, SO_SNDBUF,
               reinterpret_cast<const char*>(&bufSize), sizeof(bufSize));

    // ── 设置非阻塞（发送即忘） ──────────────────────────
    u_long nonBlocking = 1;
    ioctlsocket(m_socket, FIONBIO, &nonBlocking);

    // ── 解析目标地址 ─────────────────────────────────────
    std::memset(&m_targetAddr, 0, sizeof(m_targetAddr));
    m_targetAddr.sin_family = AF_INET;
    m_targetAddr.sin_port   = htons(port);

    if (inet_pton(AF_INET, hostIp.c_str(), &m_targetAddr.sin_addr) != 1) {
        SX_LOG_ERROR("[UdpReplySender] inet_pton 失败 ('{}': {}",
                     hostIp, WSAGetLastError());
        Cleanup();
        return false;
    }

    m_initialized = true;
    SX_LOG_INFO("[UdpReplySender] 就绪: 发送到 {}:{}", hostIp, port);
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  SendReplies（热路径）
// ═══════════════════════════════════════════════════════════════

bool UdpReplySender::SendReplies(uint32_t frameId,
                                  const std::vector<Detection>& dets) {
    if (!m_initialized) return false;

    // 检测结果上限为每次回复最大数量
    uint16_t numDets = static_cast<uint16_t>(
        std::min(dets.size(), static_cast<size_t>(MAX_DETS_PER_REPLY)));

    // ── 在栈上构建数据包 ─────────────────────────────────
    constexpr size_t kMaxPacket = sizeof(ReplyHeader)
                                + MAX_DETS_PER_REPLY * sizeof(DetectionRaw);
    alignas(64) uint8_t packetBuf[kMaxPacket];

    auto* header = reinterpret_cast<ReplyHeader*>(packetBuf);
    header->magic   = REPLY_MAGIC;
    header->frameId = frameId;
    header->numDets = numDets;
    // 填充字节已由 ReplyHeader 默认初始化清零

    auto* rawDets = reinterpret_cast<DetectionRaw*>(packetBuf + sizeof(ReplyHeader));
    for (uint16_t i = 0; i < numDets; ++i) {
        rawDets[i].x1         = dets[i].x1;
        rawDets[i].y1         = dets[i].y1;
        rawDets[i].x2         = dets[i].x2;
        rawDets[i].y2         = dets[i].y2;
        rawDets[i].confidence = dets[i].confidence;
        rawDets[i].classId    = static_cast<uint32_t>(dets[i].classId);
    }

    int totalBytes = static_cast<int>(
        sizeof(ReplyHeader) + numDets * sizeof(DetectionRaw));

    int sent = sendto(m_socket,
                      reinterpret_cast<const char*>(packetBuf),
                      totalBytes,
                      0,
                      reinterpret_cast<const sockaddr*>(&m_targetAddr),
                      sizeof(m_targetAddr));

    if (sent == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            SX_LOG_ERROR("[UdpReplySender] sendto 失败: {}", err);
        }
        return false;
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════
//  Cleanup（清理）
// ═══════════════════════════════════════════════════════════════

void UdpReplySender::Cleanup() {
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
