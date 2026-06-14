#pragma once

// ─── ReassemblyBuffer ─────────────────────────────────────────
// Out-of-order UDP chunk reassembly engine.
//
// Follows HOST_SPEC.md Section 3 algorithm exactly:
//   · Chunks are placed at offset = chunkIndex * MAX_PAYLOAD_SIZE
//   · Duplicate chunks (via receivedMask) are silently dropped
//   · Newer frameId → old partial frame is discarded immediately
//     (the iron law of low-latency vision — never wait for stragglers)
//   · Stale packets (frameId behind expectedFrameId) are dropped
//
// All buffers are pre-allocated to worst-case size to avoid
// heap allocations in the receive hot path.

#include "PacketHeader.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace SynapseX {

// ── Worst-case pre-allocation constants ─────────────────────
// These ensure zero heap allocation during steady-state operation.
constexpr uint32_t kRawFrameSize      = 640 * 640 * 4;   // BGRA: 1,638,400 bytes
constexpr uint32_t kMaxCompressedSize = 1644841;          // LZ4_compressBound(1638400)
constexpr uint16_t kMaxChunks         = 1175;             // ceil(1644841 / 1400)

// ── Frame ID wrap-around-safe comparison ────────────────────
// Returns true if `a` is newer than `b`, correctly handling
// uint32_t wrap-around (at 170 Hz this wraps after ~290 days).
inline bool IsNewerFrameId(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) > 0;
}

// ═══════════════════════════════════════════════════════════════
//  ReassemblyBuffer
// ═══════════════════════════════════════════════════════════════
struct ReassemblyBuffer {
    // ── Current frame state ──────────────────────────────────
    uint32_t expectedFrameId = 0xFFFFFFFF;  // sentinel: no active frame
    uint32_t totalSize       = 0;           // compressed size for active frame
    uint16_t totalChunks     = 0;           // expected chunk count
    uint16_t chunksReceived  = 0;           // unique chunks collected so far

    // ── Pre-allocated storage ────────────────────────────────
    // receivedMask: bitmask tracking which chunks have arrived.
    // data:         reassembly buffer for compressed payload.
    // Both are pre-allocated to worst-case size.
    std::vector<bool>    receivedMask;
    std::vector<uint8_t> data;

    // ── Constructor: pre-allocate to worst-case ──────────────
    ReassemblyBuffer() {
        receivedMask.reserve(kMaxChunks);
        data.reserve(kMaxCompressedSize);
    }

    // ── StartFrame ───────────────────────────────────────────
    // Begin collecting a new frame. Any partial old frame is
    // irrevocably discarded (the caller must have already counted
    // it as dropped if it was incomplete).
    void StartFrame(uint32_t frameId, uint32_t totalSz, uint16_t totalCh) {
        expectedFrameId = frameId;
        totalSize       = totalSz;
        totalChunks     = totalCh;
        chunksReceived  = 0;

        // Enlarge data buffer only if this frame's compressed
        // payload is larger than our pre-allocation. In practice,
        // compressed frames are 30–400 KB, so this rarely fires.
        if (data.size() < totalSz) {
            data.resize(totalSz);
        }

        // Reset received mask (vector<bool> assign: O(N) but
        // kMaxChunks = 1175 bits ≈ 147 bytes — trivially fast).
        receivedMask.assign(totalCh, false);
    }

    // ── InsertChunk ──────────────────────────────────────────
    // Copy `payload` (payloadSize bytes) into the reassembly
    // buffer at offset = chunkIndex * MAX_PAYLOAD_SIZE.
    // Returns true if the chunk was new (not a duplicate).
    // Returns false for: duplicate chunk, out-of-range index.
    inline bool InsertChunk(uint16_t chunkIndex,
                            const uint8_t* payload,
                            uint16_t payloadSize) {
        if (chunkIndex >= totalChunks) return false;
        if (receivedMask[chunkIndex]) return false;  // duplicate — drop

        const uint32_t offset = static_cast<uint32_t>(chunkIndex) * MAX_PAYLOAD_SIZE;
        std::memcpy(data.data() + offset, payload, payloadSize);

        receivedMask[chunkIndex] = true;
        ++chunksReceived;
        return true;
    }

    // ── IsComplete ───────────────────────────────────────────
    inline bool IsComplete() const {
        return totalChunks > 0 && chunksReceived == totalChunks;
    }

    // ── HasActiveFrame ───────────────────────────────────────
    inline bool HasActiveFrame() const {
        return expectedFrameId != 0xFFFFFFFF;
    }

    // ── Reset ────────────────────────────────────────────────
    // Fully discard all state. Used on cleanup or after a
    // completed frame is consumed.
    void Reset() {
        expectedFrameId = 0xFFFFFFFF;
        totalSize       = 0;
        totalChunks     = 0;
        chunksReceived  = 0;
        receivedMask.clear();
        // data is intentionally preserved (reused capacity)
    }
};

} // namespace SynapseX
