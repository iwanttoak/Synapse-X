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
//   · LZ4_decompress_safe output verified against 640×640×4 = 1,638,400.
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
    // outFrame: receives 640×640×4 BGRA pixel data on success.
    // outFrameId: receives the monotonic frame counter on success.
    //
    // Returns false if no complete frame is ready (normal — caller
    // should loop at high frequency).
    bool TryReceive(std::vector<uint8_t>& outFrame, uint32_t& outFrameId);

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
    // Returns true if decompression succeeded and output == 1,638,400 bytes.
    bool DecompressCurrentFrame(std::vector<uint8_t>& outFrame);

    // ── Socket ─────────────────────────────────────────────
    SOCKET m_socket      = INVALID_SOCKET;
    bool   m_initialized = false;
    bool   m_wsaStarted  = false;

    // ── Receive buffer (stack / member, NOT heap per-packet) ─
    // 65536 bytes: well above any possible UDP datagram (~1416 bytes),
    // and comfortably fits multiple queued packets if the OS delivers
    // them in a single recvfrom call (unlikely with UDP, but safe).
    static constexpr int kRecvBufSize = 65536;
    alignas(64) uint8_t m_recvBuf[kRecvBufSize];

    // ── Decompression output buffer ────────────────────────
    // Pre-allocated to exactly 640×640×4 = 1,638,400 bytes.
    // Reused every frame — no allocation in TryReceive.
    std::vector<uint8_t> m_decompressBuf;

    // ── Reassembly state ───────────────────────────────────
    ReassemblyBuffer m_buffer;

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
