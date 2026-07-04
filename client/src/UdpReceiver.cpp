// ─── UdpReceiver.cpp ───────────────────────────────────────────
// 非阻塞UDP接收循环 + 乱序重组 + LZ4解压。
//
// 热路径：TryReceive() 排空所有排队的数据报，使用 ReassemblyBuffer
// 重组数据块，并解压完整帧。
// 所有缓冲区预分配 — 每帧零堆分配。
//
// UDP 数据报布局（更新的 20 字节 PacketHeader）：
//   ┌─────────────────────────┬────────────────────────────────┐
//   │     PacketHeader        │       payload (LZ4 分片)       │
//   │      20 字节           │        ≤ MAX_PAYLOAD_SIZE      │
//   └─────────────────────────┴────────────────────────────────┘

#include "UdpReceiver.h"
#include "PacketHeader.h"

#include <lz4.h>

#include <cstdio>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace SynapseX {

// ═══════════════════════════════════════════════════════════════
//  生命周期
// ═══════════════════════════════════════════════════════════════

UdpReceiver::~UdpReceiver() {
    Cleanup();
}

bool UdpReceiver::Initialize(uint16_t port) {
    if (m_initialized) Cleanup();

    // ── WinSock 启动 ────────────────────────────────────
    WSADATA wsaData = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "[UdpReceiver] WSAStartup FAILED: %d\n", WSAGetLastError());
        return false;
    }
    m_wsaStarted = true;

    // ── 创建UDP套接字 ──────────────────────────────────────
    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == INVALID_SOCKET) {
        fprintf(stderr, "[UdpReceiver] socket() FAILED: %d\n", WSAGetLastError());
        Cleanup();
        return false;
    }

    // ── 设置非阻塞模式 ──────────────────────────────────
    // 这很关键：我们希望排空所有排队的数据包，
    // 而不会在 recvfrom 上阻塞。
    u_long nonBlocking = 1;
    if (ioctlsocket(m_socket, FIONBIO, &nonBlocking) != 0) {
        fprintf(stderr, "[UdpReceiver] ioctlsocket(FIONBIO) FAILED: %d\n",
                WSAGetLastError());
        Cleanup();
        return false;
    }

    // ── 扩大接收缓冲区 ─────────────────────────────────
    int bufSize = 256 * 1024;  // 256 KB
    setsockopt(m_socket, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&bufSize), sizeof(bufSize));

    // ── 绑定到本地端口 ─────────────────────────────────
    sockaddr_in localAddr = {};
    localAddr.sin_family      = AF_INET;
    localAddr.sin_port        = htons(port);
    localAddr.sin_addr.s_addr = INADDR_ANY;  // 监听所有接口

    if (bind(m_socket, reinterpret_cast<const sockaddr*>(&localAddr),
             sizeof(localAddr)) == SOCKET_ERROR) {
        fprintf(stderr, "[UdpReceiver] bind(:%u) FAILED: %d\n",
                port, WSAGetLastError());
        Cleanup();
        return false;
    }

    // m_decompressBuf 在此处不预先调整大小 — 它在第一个完整帧时
    // 根据 PacketHeader 的宽度/高度惰性增长。

    m_initialized = true;
    fprintf(stderr, "[UdpReceiver] Ready -- listening on 0.0.0.0:%u, "
            "非阻塞, 接收缓冲区 %d KB, 动态ROI\n",
            port, bufSize / 1024);
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  TryReceive（热路径）
// ═══════════════════════════════════════════════════════════════

bool UdpReceiver::TryReceive(std::vector<uint8_t>& outFrame,
                             uint32_t& outFrameId) {
    if (!m_initialized) return false;

    bool anyFrameCompleted = false;

    // ── 排空所有排队的UDP数据报 ──────────────────────────
    while (true) {
        sockaddr_in fromAddr = {};
        int fromLen = sizeof(fromAddr);
        int bytes = recvfrom(m_socket,
                             reinterpret_cast<char*>(m_recvBuf),
                             kRecvBufSize,
                             0,
                             reinterpret_cast<sockaddr*>(&fromAddr),
                             &fromLen);

        if (bytes == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                // 套接字缓冲区中没有更多数据 — 退出排空循环。
                break;
            }
            // 真实错误（稳态下不太可能）
            fprintf(stderr, "[UdpReceiver] recvfrom ERROR: %d\n", err);
            break;
        }

        if (bytes < static_cast<int>(sizeof(PacketHeader))) {
            // 数据报过小 — 静默丢弃。
            continue;
        }

        m_totalPackets++;
        m_totalBytes += static_cast<uint64_t>(bytes);

        bool frameCompleted = ProcessDatagram(m_recvBuf, bytes);
        if (frameCompleted) {
            anyFrameCompleted = true;
        }
    }

    // ── 返回最新的完整帧 ────────────────────────────────
    if (anyFrameCompleted) {
        // m_decompressBuf 已由 DecompressCurrentFrame 调整大小并填充。
        // 原样复制给调用方。
        outFrame = m_decompressBuf;
        outFrameId = m_lastDecodedFrameId;
        return true;
    }

    return false;
}

// ═══════════════════════════════════════════════════════════════
//  ProcessDatagram — 逐数据包逻辑
// ═══════════════════════════════════════════════════════════════

bool UdpReceiver::ProcessDatagram(const uint8_t* data, int len) {
    // ── 1. 解析头部 ─────────────────────────────────────────
    const auto* header = reinterpret_cast<const PacketHeader*>(data);

    // ── 2. 验证魔数 ─────────────────────────────────────────
    if (header->magic != PROTOCOL_MAGIC) {
        // 不是我们的协议 — 静默丢弃。
        return false;
    }

    // ── 3. 提取负载 ─────────────────────────────────────────
    const uint8_t* payload = data + sizeof(PacketHeader);
    const uint16_t payloadSize = header->payloadSize;

    // 安全检查：负载必须适合收到的数据报
    if (static_cast<int>(sizeof(PacketHeader) + payloadSize) > len) {
        fprintf(stderr, "[UdpReceiver] Truncated datagram: header says %u payload, "
                "got %d total bytes\n", payloadSize, len);
        return false;
    }

    // ── 4. 帧转换逻辑 ───────────────────────────────────────
    const uint32_t frameId      = header->frameId;
    const uint32_t totalSize    = header->totalSize;
    const uint16_t totalChunks  = header->totalChunks;
    const uint16_t chunkIndex   = header->chunkIndex;
    const uint16_t frameWidth   = header->width;
    const uint16_t frameHeight  = header->height;

    const uint8_t modelId = header->modelId;

    if (!m_buffer.HasActiveFrame()) {
        // 第一帧 — 开始收集。
        m_buffer.StartFrame(frameId, totalSize, totalChunks,
                            frameWidth, frameHeight);
        m_activeFrameIncomplete = true;
        g_targetModelId.store(modelId, std::memory_order_relaxed);
    } else if (IsNewerFrameId(frameId, m_buffer.expectedFrameId)) {
        // 更新的帧到达 — 刷新旧的不完整帧。
        if (m_activeFrameIncomplete) {
            m_totalDropped++;
        }
        m_buffer.StartFrame(frameId, totalSize, totalChunks,
                            frameWidth, frameHeight);
        m_activeFrameIncomplete = true;
        g_targetModelId.store(modelId, std::memory_order_relaxed);
    } else if (frameId != m_buffer.expectedFrameId) {
        // 来自旧帧的过期数据包 — 丢弃。
        return false;
    }
    // else: frameId == expectedFrameId — 继续收集。

    // ── 5. 插入数据块 ───────────────────────────────────────
    bool inserted = m_buffer.InsertChunk(chunkIndex, payload, payloadSize);
    if (!inserted) {
        // 重复或超出范围 — 已处理。
        return false;
    }

    // ── 6. 检查完成状态 ─────────────────────────────────────
    if (m_buffer.IsComplete()) {
        m_activeFrameIncomplete = false;
        // 检测帧ID间隔以计算丢帧率。
        if (m_hasDecodedAnyFrame) {
            // 统计跳过的帧ID（主机已发送但我们从未开始）。
            // 使用 int32_t 算术正确处理环绕。
            int32_t gap = static_cast<int32_t>(frameId - m_lastDecodedFrameId) - 1;
            if (gap > 0) {
                m_totalDropped += static_cast<uint64_t>(gap);
            }
        }

        // 解压到 m_decompressBuf（每帧重用）。
        // 宽度/高度已从 StartFrame 存储在 m_buffer 中。
        bool ok = DecompressCurrentFrame(m_decompressBuf);
        if (ok) {
            m_totalFramesReceived++;
            m_lastDecodedFrameId = frameId;
            m_hasDecodedAnyFrame = true;

            // 缓存尺寸供调用方解释 outFrame
            m_lastFrameWidth  = m_buffer.frameWidth;
            m_lastFrameHeight = m_buffer.frameHeight;

            // 重置缓冲区以准备下一帧。
            m_buffer.Reset();

            return true;  // 信号：帧已完成
        } else {
            // 解压失败 — 计为丢弃，重置，继续。
            m_totalDropped++;
            m_buffer.Reset();
        }
    }

    return false;
}

// ═══════════════════════════════════════════════════════════════
//  DecompressCurrentFrame（解压当前帧）
// ═══════════════════════════════════════════════════════════════

bool UdpReceiver::DecompressCurrentFrame(std::vector<uint8_t>& outFrame) {
    const int compressedSize = static_cast<int>(m_buffer.totalSize);
    const uint32_t rawSize = m_buffer.GetRawFrameSize();

    // 确保输出缓冲区足够大（缩小时重用容量）
    outFrame.resize(rawSize);

    int result = LZ4_decompress_safe(
        reinterpret_cast<const char*>(m_buffer.data.data()),
        reinterpret_cast<char*>(outFrame.data()),
        compressedSize,
        static_cast<int>(outFrame.size())
    );

    const int expectedSize = static_cast<int>(rawSize);
    if (result != expectedSize) {
        if (result < 0) {
            fprintf(stderr, "[UdpReceiver] LZ4_decompress_safe ERROR: %d "
                    "(compressed=%d bytes, raw=%ux%ux4=%u, frameId=%u)\n",
                    result, compressedSize,
                    m_buffer.frameWidth, m_buffer.frameHeight, rawSize,
                    m_buffer.expectedFrameId);
        } else {
            fprintf(stderr, "[UdpReceiver] LZ4 size mismatch: got %d, expected %d "
                    "(raw=%ux%ux4=%u, compressed=%d bytes, frameId=%u)\n",
                    result, expectedSize,
                    m_buffer.frameWidth, m_buffer.frameHeight, rawSize,
                    compressedSize, m_buffer.expectedFrameId);
        }
        return false;
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════
//  Cleanup（清理）
// ═══════════════════════════════════════════════════════════════

void UdpReceiver::Cleanup() {
    if (m_socket != INVALID_SOCKET) {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }
    if (m_wsaStarted) {
        WSACleanup();
        m_wsaStarted = false;
    }
    m_initialized = false;
    m_buffer.Reset();
    m_lastFrameWidth  = 0;
    m_lastFrameHeight = 0;
}

} // namespace SynapseX
