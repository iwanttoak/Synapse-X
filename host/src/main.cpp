// ─── Synapse-X Host -- Production main loop ─────────────────
//
// Fixed 170 Hz pipeline:
//   DXGI capture (try every tick)
//     -> new frame?  LZ4 compress + update cache
//     -> no change?  re-send cached compressed frame
//   UDP fragment & send (every tick)
//
// Usage:
//   SynapseX_Host.exe [target_ip] [port] [roi_w] [roi_h]
//   Defaults: 192.168.100.2  8888   640     640

#include "DxgiCapturer.h"
#include "Lz4Compressor.h"
#include "UdpSender.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <atomic>
#include <thread>

#include <windows.h>    // timeBeginPeriod / timeEndPeriod
#include <mmsystem.h>   // (link winmm.lib)

#pragma comment(lib, "winmm.lib")

static std::atomic<bool> g_running{true};

using Clock     = std::chrono::high_resolution_clock;
using TimePoint = Clock::time_point;

static inline double ToMs(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

struct PerfStats {
    int    captured    = 0;    // new frames from DXGI this window
    int    sent        = 0;    // total UDP sends this window
    double sumCapture  = 0.0;  // only measured on new frames
    double sumCompress = 0.0;
    double sumSend     = 0.0;

    void reset() {
        captured    = 0;
        sent        = 0;
        sumCapture  = 0.0;
        sumCompress = 0.0;
        sumSend     = 0.0;
    }
};

int main(int argc, char* argv[]) {
    // ── Parse arguments ──────────────────────────────────
    const char* targetIp   = (argc > 1) ? argv[1] : "192.168.100.2";
    uint16_t    targetPort = (argc > 2) ? static_cast<uint16_t>(std::atoi(argv[2])) : 8888;
    int         roiW       = (argc > 3) ? std::atoi(argv[3]) : 640;
    int         roiH       = (argc > 4) ? std::atoi(argv[4]) : 640;

    if (roiW < 64 || roiH < 64 || roiW > 4096 || roiH > 4096) {
        fprintf(stderr, "[FATAL] Invalid ROI: %dx%d (min 64, max 4096)\n", roiW, roiH);
        return 1;
    }

    int rawSize = roiW * roiH * 4;

    // ── Fixed 170 Hz cadence ──────────────────────────────
    constexpr double kTargetFps  = 170.0;
    constexpr double kTargetMs   = 1000.0 / kTargetFps;   // ~5.882 ms
    const auto       kInterval   = std::chrono::duration<double, std::milli>(kTargetMs);

    // Boost Windows timer resolution.
    // Default is 15.6 ms — way too coarse for 5.88 ms ticks.
    timeBeginPeriod(1);

    fprintf(stderr, "============================================\n");
    fprintf(stderr, "  Synapse-X Host -- Fixed %.0f Hz Pipeline\n", kTargetFps);
    fprintf(stderr, "  Target: %s:%u\n", targetIp, targetPort);
    fprintf(stderr, "  ROI:    %dx%d  (%.2f MB raw)\n",
            roiW, roiH, rawSize / (1024.0 * 1024.0));
    fprintf(stderr, "  Budget: %.2f ms per frame\n", kTargetMs);
    fprintf(stderr, "============================================\n\n");

    // ── Stage 0: Init all modules ────────────────────────
    SynapseX::DxgiCapturer capturer;
    if (!capturer.Initialize(roiW, roiH)) {
        fprintf(stderr, "[FATAL] DxgiCapturer init FAILED.\n");
        return 1;
    }

    SynapseX::Lz4Compressor compressor;
    if (!compressor.Initialize(rawSize)) {
        fprintf(stderr, "[FATAL] Lz4Compressor init FAILED.\n");
        return 1;
    }

    SynapseX::UdpSender sender;
    if (!sender.Initialize(targetIp, targetPort)) {
        fprintf(stderr, "[FATAL] UdpSender init FAILED.\n");
        return 1;
    }

    fprintf(stderr, "[INFO] All modules initialized. Starting main loop...\n");
    fprintf(stderr, "[INFO] Press Ctrl+C to stop.\n\n");

    // ── Main loop state ──────────────────────────────────
    std::vector<uint8_t> rawBuffer;
    rawBuffer.reserve(rawSize);

    std::vector<uint8_t> compressedBuffer;
    compressedBuffer.reserve(SynapseX::Lz4Compressor::GetMaxOutputSize(rawSize));

    // Cached compressed frame — re-sent when desktop is idle.
    std::vector<uint8_t> cachedCompressed;
    cachedCompressed.reserve(SynapseX::Lz4Compressor::GetMaxOutputSize(rawSize));
    bool hasCachedFrame = false;

    uint32_t    frameId       = 0;
    PerfStats   stats;
    TimePoint   windowStart   = Clock::now();
    int64_t     totalSent     = 0;
    TimePoint   sessionStart  = Clock::now();

    const uint16_t roiW16 = static_cast<uint16_t>(roiW);
    const uint16_t roiH16 = static_cast<uint16_t>(roiH);

    auto nextTick = Clock::now();

    // ═══════════════════════════════════════════════════════
    //  MAIN LOOP — Fixed 170 Hz
    // ═══════════════════════════════════════════════════════
    while (g_running) {
        // ── Stage 1: Capture (try every tick) ─────────────
        auto t0 = Clock::now();
        bool gotFrame = capturer.CaptureFrame(rawBuffer);
        auto t1 = Clock::now();

        if (gotFrame) {
            // ── Stage 2: Compress (only on new content) ───
            auto t2a = Clock::now();
            bool ok = compressor.Compress(rawBuffer.data(),
                                           static_cast<int>(rawBuffer.size()),
                                           compressedBuffer);
            auto t2b = Clock::now();

            if (ok) {
                // Update cache for future re-sends
                cachedCompressed = compressedBuffer;
                hasCachedFrame   = true;

                stats.captured++;
                stats.sumCapture  += ToMs(t1  - t0);
                stats.sumCompress += ToMs(t2b - t2a);
            }
        }

        // ── Stage 3: Send (ALWAYS — reuses cache if idle) ──
        if (hasCachedFrame) {
            auto t3a = Clock::now();
            bool sent = sender.SendCompressedFrame(
                cachedCompressed.data(),
                static_cast<uint32_t>(cachedCompressed.size()),
                frameId,
                roiW16, roiH16);
            auto t3b = Clock::now();

            if (sent) totalSent++;

            stats.sent++;
            stats.sumSend += ToMs(t3b - t3a);
            frameId++;
        }

        // ── Per-second stats report ──────────────────────
        double elapsed = ToMs(Clock::now() - windowStart) / 1000.0;
        if (elapsed >= 1.0) {
            double sendFps    = stats.sent / elapsed;
            double captureFps = stats.captured / elapsed;
            double avgCapture = stats.captured > 0 ? stats.sumCapture  / stats.captured : 0.0;
            double avgCompress= stats.captured > 0 ? stats.sumCompress / stats.captured : 0.0;
            double avgSend    = stats.sent     > 0 ? stats.sumSend     / stats.sent     : 0.0;

            fprintf(stderr,
                "---- per-second stats --------------------------------\n"
                "  Send FPS: %6.1f  |  capture FPS: %6.1f  |  "
                "fresh: %d  cache: %d  |  total: %lld\n"
                "  capture: %8.3f ms  |  compress: %8.3f ms  |  "
                "send: %8.3f ms\n"
                "  budget: %5.2f ms @%.0f Hz\n",
                sendFps, captureFps,
                stats.captured, stats.sent - stats.captured,
                (long long)totalSent,
                avgCapture, avgCompress, avgSend,
                kTargetMs, kTargetFps);

            stats.reset();
            windowStart = Clock::now();
        }

        // ── Maintain 170 Hz cadence ──────────────────────
        nextTick += std::chrono::duration_cast<Clock::duration>(kInterval);
        auto now = Clock::now();
        if (nextTick > now) {
            std::this_thread::sleep_until(nextTick);
        } else {
            // Fell behind schedule — reset to avoid death-spiral
            nextTick = now;
        }
    }

    timeEndPeriod(1);

    double sessionSec = ToMs(Clock::now() - sessionStart) / 1000.0;
    fprintf(stderr, "\n[DONE] Session ended. %.1f sec, %lld frames sent, avg %.1f FPS\n",
            sessionSec, (long long)totalSent,
            sessionSec > 0.0 ? totalSent / sessionSec : 0.0);

    capturer.Cleanup();
    sender.Cleanup();
    return 0;
}
