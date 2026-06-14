// ─── Synapse-X Host -- Production main loop ─────────────────
//
// Pipeline (170 Hz target):
//   DXGI center-ROI capture  ->  LZ4 compress  ->  UDP fragment & send
//
// Usage:
//   SynapseX_Host.exe [target_ip] [port] [roi_w] [roi_h]
//   Defaults: 192.168.100.2  8888   640     640
//
// Stats printed every second: actual FPS + per-stage avg latency.

#include "DxgiCapturer.h"
#include "Lz4Compressor.h"
#include "UdpSender.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <atomic>
#include <thread>

static std::atomic<bool> g_running{true};

using Clock     = std::chrono::high_resolution_clock;
using TimePoint = Clock::time_point;

static inline double ToMs(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

struct PerfStats {
    int    captured    = 0;
    int    sent        = 0;
    double sumCapture  = 0.0;
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

    fprintf(stderr, "============================================\n");
    fprintf(stderr, "  Synapse-X Host -- Production Pipeline\n");
    fprintf(stderr, "  Target: %s:%u\n", targetIp, targetPort);
    fprintf(stderr, "  ROI:    %dx%d  (%.2f MB raw)\n", roiW, roiH, rawSize / (1024.0 * 1024.0));
    fprintf(stderr, "  Target FPS: 170 Hz\n");
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
    std::vector<uint8_t> compressedBuffer;
    rawBuffer.reserve(rawSize);
    compressedBuffer.reserve(SynapseX::Lz4Compressor::GetMaxOutputSize(rawSize));

    uint32_t    frameId       = 0;
    PerfStats   stats;
    TimePoint   windowStart   = Clock::now();
    int64_t     totalSent     = 0;
    TimePoint   sessionStart  = Clock::now();

    const uint16_t roiW16 = static_cast<uint16_t>(roiW);
    const uint16_t roiH16 = static_cast<uint16_t>(roiH);

    // ═══════════════════════════════════════════════════════
    //  MAIN LOOP
    // ═══════════════════════════════════════════════════════
    while (g_running) {
        auto t0 = Clock::now();
        bool gotFrame = capturer.CaptureFrame(rawBuffer);
        auto t1 = Clock::now();

        if (gotFrame) {
            auto t2a = Clock::now();
            bool ok = compressor.Compress(rawBuffer.data(),
                                           static_cast<int>(rawBuffer.size()),
                                           compressedBuffer);
            auto t2b = Clock::now();

            if (ok) {
                auto t3a = Clock::now();
                bool sent = sender.SendCompressedFrame(
                    compressedBuffer.data(),
                    static_cast<uint32_t>(compressedBuffer.size()),
                    frameId,
                    roiW16,
                    roiH16);
                auto t3b = Clock::now();

                if (sent) totalSent++;

                stats.captured++;
                stats.sent       += (sent ? 1 : 0);
                stats.sumCapture  += ToMs(t1  - t0);
                stats.sumCompress += ToMs(t2b - t2a);
                stats.sumSend     += ToMs(t3b - t3a);
                frameId++;
            }
        } else {
            std::this_thread::yield();
        }

        // ── Per-second stats report ──────────────────────
        double elapsed = ToMs(Clock::now() - windowStart) / 1000.0;
        if (elapsed >= 1.0) {
            double fps        = stats.captured / elapsed;
            double avgCapture = stats.captured > 0 ? stats.sumCapture  / stats.captured : 0.0;
            double avgCompress= stats.captured > 0 ? stats.sumCompress / stats.captured : 0.0;
            double avgSend    = stats.sent     > 0 ? stats.sumSend     / stats.sent     : 0.0;
            double avgTotal   = avgCapture + avgCompress + avgSend;

            fprintf(stderr,
                "---- per-second stats --------------------------------\n"
                "  FPS: %7.1f  |  sent: %5d / %5d frames  |  total: %lld\n"
                "  capture: %8.3f ms  |  compress: %8.3f ms  |  send: %8.3f ms\n"
                "  pipeline total: %5.2f ms  (budget: 5.88 ms @170Hz)\n",
                fps, stats.sent, stats.captured, (long long)totalSent,
                avgCapture, avgCompress, avgSend,
                avgTotal);

            stats.reset();
            windowStart = Clock::now();
        }
    }

    double sessionSec = ToMs(Clock::now() - sessionStart) / 1000.0;
    fprintf(stderr, "\n[DONE] Session ended. %.1f sec, %lld frames sent, avg %.1f FPS\n",
            sessionSec, (long long)totalSent,
            sessionSec > 0.0 ? totalSent / sessionSec : 0.0);

    capturer.Cleanup();
    sender.Cleanup();
    return 0;
}