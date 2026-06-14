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
// All buffers are pre-allocated to worst-case size (4096×4096 ROI)
// to avoid heap allocations in the receive hot path.  In steady state
// StartFrame() only reallocates data if the compressed frame exceeds
// the current capacity, which with the worst-case pre-allocation
// will never happen for any realistic ROI.
//
// Supports dynamic ROI: frameWidth / frameHeight are carried from
// the PacketHeader through to decompression verification.

#include "PacketHeader.h"

#include <lz4.h>
#include <algorithm>

#include <cstdint>
#include <cstring>
#include <vector>

namespace SynapseX {

// ── Worst-case pre-allocation constants (4096 × 4096 ROI) ─────
// Host may send any ROI from 416² through 4096².  We pre-allocate
// for the worst case to guarantee zero heap allocation per frame.
constexpr uint32_t kMaxRoiPixels       = 4096 * 4096;            // 16,777,216 px
constexpr uint32_t kMaxRawFrameSize    = kMaxRoiPixels * 4;      // 67,108,864 bytes
constexpr uint32_t kMaxCompressedSize  = 67372052;               // LZ4_compressBound(67108864)
constexpr uint16_t kMaxChunks          = 48123;                  // ceil(67372052 / 1400)

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
    uint16_t frameWidth      = 0;           // ROI width  (from PacketHeader)
    uint16_t frameHeight     = 0;           // ROI height (from PacketHeader)

    // ── Convenience ─────────────────────────────────────────
    uint32_t GetRawFrameSize() const {
        return static_cast<uint32_t>(frameWidth) *
               static_cast<uint32_t>(frameHeight) * 4;
    }

    // ── Pre-allocated storage ────────────────────────────────
    // receivedMask: bitmask tracking which chunks have arrived.
    // data:         reassembly buffer for compressed payload.
    // Both are pre-allocated to worst-case size (4096² ROI).
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
    //
    // width/height are extracted from PacketHeader and carried
    // through to decompression verification.
    void StartFrame(uint32_t frameId,
                    uint32_t totalSz,
                    uint16_t totalCh,
                    uint16_t width,
                    uint16_t height) {
        expectedFrameId = frameId;
        totalSize       = totalSz;
        totalChunks     = totalCh;
        chunksReceived  = 0;
        frameWidth      = width;
        frameHeight     = height;

        // Enlarge data buffer only if this frame's compressed
        // payload exceeds pre-allocated capacity.  With 64 MiB
        // worst-case pre-allocation, this never fires in practice.
        if (data.size() < totalSz) {
            data.resize(totalSz);
        }

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
        frameWidth      = 0;
        frameHeight     = 0;
        receivedMask.clear();
        // data is intentionally preserved (reused capacity)
    }
};

} // namespace SynapseX
