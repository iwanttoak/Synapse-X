#pragma once

// ─── UdpReceiver ──────────────────────────────────────────────
// Non-blocking UDP receive + LZ4 decompress pipeline.
//
// Lifecycle:
//   1. Initialize(port) — bind UDP socket, pre-allocate buffers.
//   2. TryReceive() hot loop — drain all available datagrams,
//      reassemble out-of-order chunks, decompress complete frames.
//   3. Cleanup() — close socket, release WinSock.
//
// Design constraints:
//   · Non-blocking socket with eager drain — process all queued
//     packets in one TryReceive() call.
//   · Zero heap allocation in TryReceive() after initial pre-allocation.
//   · Dynamic ROI: width/height extracted from PacketHeader each frame,
//     LZ4 output verified against width×height×4.
//   · Frame ID wrap-around handled safely (int32_t difference).
//
// The caller is responsible for calling TryReceive() at high
// frequency (ideally every ~1 ms or tighter).

#include "ReassemblyBuffer.h"

#include <cstdint>
#include <vector>
#include <winsock2.h>

namespace SynapseX {

class UdpReceiver {
public:
    UdpReceiver() = default;
    ~UdpReceiver();

    // Non-copyable, non-movable (owns socket)
    UdpReceiver(const UdpReceiver&) = delete;
    UdpReceiver& operator=(const UdpReceiver&) = delete;
    UdpReceiver(UdpReceiver&&) = delete;
    UdpReceiver& operator=(UdpReceiver&&) = delete;

    // ── Lifecycle ─────────────────────────────────────────

    // Bind UDP socket to `port`, pre-allocate all buffers.
    // No ROI size needed — decompress buffer is resized per-frame
    // based on the width/height in each PacketHeader.
    // Returns true on success.
    bool Initialize(uint16_t port = 8888);

    // Close socket and release WinSock resources.
    void Cleanup();

    bool IsInitialized() const { return m_initialized; }

    // ── Hot path: receive, reassemble, decompress ──────────

    // Drain all queued UDP datagrams from the socket buffer.
    // If at least one complete frame was assembled and successfully
    // decompressed, the MOST RECENT one is returned in outFrame
    // with its frameId in outFrameId, and the function returns true.
    //
    // outFrame: receives width×height×4 BGRA pixel data on success.
    // outFrameId: receives the monotonic frame counter on success.
    // Use GetLastFrameWidth()/GetLastFrameHeight() to interpret outFrame.
    //
    // Returns false if no complete frame is ready (normal — caller
    // should loop at high frequency).
    bool TryReceive(std::vector<uint8_t>& outFrame, uint32_t& outFrameId);

    // ── Last frame dimensions (valid after TryReceive returns true) ─
    uint16_t GetLastFrameWidth()  const { return m_lastFrameWidth; }
    uint16_t GetLastFrameHeight() const { return m_lastFrameHeight; }

    // ── Stats ──────────────────────────────────────────────

    uint64_t GetTotalFrames()  const { return m_totalFramesReceived; }
    uint64_t GetTotalDropped() const { return m_totalDropped; }
    uint64_t GetTotalPackets() const { return m_totalPackets; }
    uint64_t GetTotalBytes()   const { return m_totalBytes; }

private:
    // Process a single raw UDP datagram.
    // Validates header, handles frame transitions, inserts chunk.
    // Returns true if this packet caused a frame to become complete.
    bool ProcessDatagram(const uint8_t* data, int len);

    // Decompress the current reassembly buffer into outFrame.
    // Dynamically resizes outFrame to frameWidth × frameHeight × 4.
    // Returns true if decompression succeeded and output matches expected size.
    bool DecompressCurrentFrame(std::vector<uint8_t>& outFrame);

    // ── Socket ─────────────────────────────────────────────
    SOCKET m_socket      = INVALID_SOCKET;
    bool   m_initialized = false;
    bool   m_wsaStarted  = false;

    // ── Receive buffer ─────────────────────────────────────
    // 65536 bytes: well above max UDP datagram (20 + 1400 = 1420 bytes).
    static constexpr int kRecvBufSize = 65536;
    alignas(64) uint8_t m_recvBuf[kRecvBufSize];

    // ── Decompression output buffer ────────────────────────
    // Resized dynamically per-frame to width×height×4.
    // Reused across frames — only reallocates if ROI grows.
    std::vector<uint8_t> m_decompressBuf;

    // ── Reassembly state ───────────────────────────────────
    ReassemblyBuffer m_buffer;

    // ── Last frame dimensions ──────────────────────────────
    uint16_t m_lastFrameWidth  = 0;
    uint16_t m_lastFrameHeight = 0;

    // ── Statistics ─────────────────────────────────────────
    uint64_t m_totalFramesReceived = 0;
    uint64_t m_totalDropped        = 0;
    uint64_t m_totalPackets        = 0;
    uint64_t m_totalBytes          = 0;

    uint32_t m_lastDecodedFrameId  = 0xFFFFFFFF;
    bool     m_hasDecodedAnyFrame  = false;

    // True while the current active frame is still incomplete.
    // Set to false when a frame completes or is abandoned.
    bool     m_activeFrameIncomplete = false;
};

} // namespace SynapseX
