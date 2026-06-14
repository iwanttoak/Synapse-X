// ─── Synapse-X Client — Test & Verification main loop ───────
//
// Pipeline:
//   UDP recv → out-of-order reassembly → LZ4 decompress → BGRA 640²
//
// Verification:
//   · Per-second FPS and drop-rate stats printed to stderr.
//   · On the 10th successfully decoded frame, saves client_test.bmp
//     (32-bit BGRA, top-down DIB) for visual confirmation.
//
// Usage:
//   .\SynapseX_Client.exe [port]
//   Default port: 8888
//
// Build:
//   cd client
//   cmake --preset windows-x64
//   cmake --build build_x64 --config RelWithDebInfo

#include "UdpReceiver.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>
#include <atomic>

// ═══════════════════════════════════════════════════════════════
//  Global control
// ═══════════════════════════════════════════════════════════════

static std::atomic<bool> g_running{true};

// ═══════════════════════════════════════════════════════════════
//  High-precision timer helpers
// ═══════════════════════════════════════════════════════════════

using Clock     = std::chrono::high_resolution_clock;
using TimePoint = Clock::time_point;

static inline double ToMs(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

// ═══════════════════════════════════════════════════════════════
//  Hand-rolled BMP writer (zero 3rd-party deps)
//  Input: BGRA pixels (matching DXGI_FORMAT_B8G8R8A8_UNORM)
//  Output: 32-bit BMP with negative biHeight (top-down DIB)
// ═══════════════════════════════════════════════════════════════

static bool SaveBgraAsBmp(const char* path,
                          const uint8_t* pixels,
                          int width,
                          int height) {
    int rowSize   = width * 4;                       // BGRA = 4 bytes/pixel
    int padSize   = (4 - (rowSize % 4)) % 4;
    int rowStride = rowSize + padSize;
    int imageSize = rowStride * height;

    // BITMAPFILEHEADER (14 bytes)
    BITMAPFILEHEADER bf = {};
    bf.bfType      = 0x4D42;                         // 'BM'
    bf.bfSize      = sizeof(BITMAPFILEHEADER)
                   + sizeof(BITMAPINFOHEADER)
                   + imageSize;
    bf.bfReserved1 = 0;
    bf.bfReserved2 = 0;
    bf.bfOffBits   = sizeof(BITMAPFILEHEADER)
                   + sizeof(BITMAPINFOHEADER);

    // BITMAPINFOHEADER (40 bytes)
    BITMAPINFOHEADER bi = {};
    bi.biSize          = sizeof(BITMAPINFOHEADER);
    bi.biWidth         = width;
    bi.biHeight        = -height;                   // negative = top-down DIB
    bi.biPlanes        = 1;
    bi.biBitCount      = 32;
    bi.biCompression   = BI_RGB;
    bi.biSizeImage     = imageSize;
    bi.biXPelsPerMeter = 0;
    bi.biYPelsPerMeter = 0;
    bi.biClrUsed       = 0;
    bi.biClrImportant  = 0;

    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[BMP] Cannot open file: %s\n", path);
        return false;
    }

    fwrite(&bf, sizeof(bf), 1, f);
    fwrite(&bi, sizeof(bi), 1, f);

    const uint8_t* row = pixels;
    uint8_t padding[4] = {0, 0, 0, 0};

    for (int y = 0; y < height; ++y) {
        fwrite(row, 1, rowSize, f);
        if (padSize > 0) {
            fwrite(padding, 1, padSize, f);
        }
        row += rowSize;
    }

    fclose(f);
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  Per-second statistics accumulator
// ═══════════════════════════════════════════════════════════════

struct Stats {
    uint64_t framesThisWindow = 0;
    uint64_t droppedThisWindow = 0;
    uint64_t packetsThisWindow = 0;
    uint64_t bytesThisWindow   = 0;
    uint64_t framesTotal       = 0;
    uint64_t droppedTotal      = 0;

    void resetWindow() {
        framesThisWindow  = 0;
        droppedThisWindow = 0;
        packetsThisWindow = 0;
        bytesThisWindow   = 0;
    }
};

// ═══════════════════════════════════════════════════════════════
//  Entry point
// ═══════════════════════════════════════════════════════════════

int main(int argc, char* argv[]) {
    // ── Parse arguments ──────────────────────────────────
    uint16_t listenPort = (argc > 1)
        ? static_cast<uint16_t>(std::atoi(argv[1]))
        : 8888;

    fprintf(stderr, "============================================\n");
    fprintf(stderr, "  Synapse-X Client — Verification Loop\n");
    fprintf(stderr, "  Listening on: 0.0.0.0:%u\n", listenPort);
    fprintf(stderr, "  Target output: 640x640 BGRA (1,638,400 bytes)\n");
    fprintf(stderr, "============================================\n\n");

    // ── Initialize UDP receiver ──────────────────────────
    SynapseX::UdpReceiver receiver;
    if (!receiver.Initialize(listenPort)) {
        fprintf(stderr, "[FATAL] UdpReceiver init FAILED.\n");
        fprintf(stderr, "  Check: is port %u already in use?\n", listenPort);
        return 1;
    }

    fprintf(stderr, "[INFO] Waiting for data from Host...\n");
    fprintf(stderr, "[INFO] Frame 10 will be saved as client_test.bmp\n");
    fprintf(stderr, "[INFO] Press Ctrl+C to stop.\n\n");

    // ── Main loop state ──────────────────────────────────
    std::vector<uint8_t> frameBuffer;
    frameBuffer.reserve(SynapseX::kRawFrameSize);

    uint32_t    receivedFrameId = 0;
    uint64_t    totalFrames     = 0;
    bool        bmpSaved        = false;
    const char* bmpPath         = "client_test.bmp";

    Stats       stats;
    TimePoint   windowStart     = Clock::now();
    TimePoint   sessionStart    = Clock::now();

    uint64_t    prevPackets     = 0;
    uint64_t    prevDropped     = 0;
    uint64_t    prevFrames      = 0;

    // ═══════════════════════════════════════════════════════
    //  MAIN RECEIVE LOOP
    // ═══════════════════════════════════════════════════════
    while (g_running) {
        // ── Try to receive a complete frame ───────────────
        bool gotFrame = receiver.TryReceive(frameBuffer, receivedFrameId);

        if (gotFrame) {
            totalFrames++;

            // ── BMP save on 10th frame ────────────────────
            if (totalFrames == 10 && !bmpSaved) {
                constexpr int ROI_W = 640;
                constexpr int ROI_H = 640;

                if (SaveBgraAsBmp(bmpPath, frameBuffer.data(), ROI_W, ROI_H)) {
                    fprintf(stderr, "\n[VERIFY] Frame #%llu (Host frameId=%u) "
                            "saved as '%s'\n",
                            static_cast<unsigned long long>(totalFrames),
                            receivedFrameId, bmpPath);
                    fprintf(stderr, "[VERIFY] Dimensions: %dx%d, 32-bit BGRA, "
                            "%zu bytes\n",
                            ROI_W, ROI_H, frameBuffer.size());

                    // Show first 4 pixels for manual check
                    if (frameBuffer.size() >= 16) {
                        const uint8_t* p = frameBuffer.data();
                        fprintf(stderr, "[VERIFY] First 4 pixels (B,G,R,A): "
                                "[%3u,%3u,%3u,%3u] [%3u,%3u,%3u,%3u] "
                                "[%3u,%3u,%3u,%3u] [%3u,%3u,%3u,%3u]\n",
                                p[0],p[1],p[2],p[3],
                                p[4],p[5],p[6],p[7],
                                p[8],p[9],p[10],p[11],
                                p[12],p[13],p[14],p[15]);
                    }
                    bmpSaved = true;
                } else {
                    fprintf(stderr, "[ERROR] BMP save failed!\n");
                }
            }
        }

        // ── Per-second stats report ──────────────────────
        double elapsed = ToMs(Clock::now() - windowStart) / 1000.0;
        if (elapsed >= 1.0) {
            uint64_t curPackets = receiver.GetTotalPackets();
            uint64_t curDropped = receiver.GetTotalDropped();
            uint64_t curFrames  = receiver.GetTotalFrames();

            uint64_t framesThisSec  = curFrames  - prevFrames;
            uint64_t droppedThisSec = curDropped - prevDropped;
            uint64_t packetsThisSec = curPackets - prevPackets;

            double fps     = framesThisSec / elapsed;
            uint64_t totalAttempted = framesThisSec + droppedThisSec;
            double dropRate = (totalAttempted > 0)
                ? (100.0 * droppedThisSec / totalAttempted)
                : 0.0;

            double MBps = (curPackets - prevPackets) > 0
                ? (receiver.GetTotalBytes() - stats.bytesThisWindow)
                  / (elapsed * 1024.0 * 1024.0)
                : 0.0;

            // Only print if we're receiving data (avoid spam when idle)
            if (packetsThisSec > 0 || framesThisSec > 0) {
                fprintf(stderr,
                    "---- per-second stats --------------------------------\n"
                    "  FPS: %7.1f  |  frames: %5llu  |  dropped: %5llu  |  "
                    "drop rate: %5.1f%%\n"
                    "  packets: %6llu/s  |  throughput: %7.2f MB/s  |  "
                    "total frames: %llu\n",
                    fps,
                    static_cast<unsigned long long>(framesThisSec),
                    static_cast<unsigned long long>(droppedThisSec),
                    dropRate,
                    static_cast<unsigned long long>(packetsThisSec),
                    MBps,
                    static_cast<unsigned long long>(curFrames));
            }

            prevFrames  = curFrames;
            prevDropped = curDropped;
            prevPackets = curPackets;

            stats.framesThisWindow  = 0;
            stats.droppedThisWindow = 0;
            stats.packetsThisWindow = 0;
            stats.bytesThisWindow   = receiver.GetTotalBytes();

            stats.framesTotal  = curFrames;
            stats.droppedTotal = curDropped;

            windowStart = Clock::now();
        }

        // ── Yield to OS if no data ────────────────────────
        // Prevents CPU spin at 100% when host is idle.
        // The host only sends when the desktop changes, so
        // there can be multi-second gaps with no traffic.
        if (!gotFrame) {
            Sleep(0);  // yield time slice
        }
    }

    // ── Final report ─────────────────────────────────────
    double sessionSec = ToMs(Clock::now() - sessionStart) / 1000.0;

    fprintf(stderr, "\n============================================\n");
    fprintf(stderr, "  Session Summary\n");
    fprintf(stderr, "============================================\n");
    fprintf(stderr, "  Duration:       %.1f sec\n", sessionSec);
    fprintf(stderr, "  Total frames:   %llu\n",
            static_cast<unsigned long long>(receiver.GetTotalFrames()));
    fprintf(stderr, "  Total dropped:  %llu\n",
            static_cast<unsigned long long>(receiver.GetTotalDropped()));
    fprintf(stderr, "  Total packets:  %llu\n",
            static_cast<unsigned long long>(receiver.GetTotalPackets()));
    fprintf(stderr, "  Total MB recv:  %.2f\n",
            receiver.GetTotalBytes() / (1024.0 * 1024.0));
    fprintf(stderr, "  Avg FPS:        %.1f\n",
            sessionSec > 0.0
                ? receiver.GetTotalFrames() / sessionSec
                : 0.0);
    fprintf(stderr, "  BMP saved:      %s\n",
            bmpSaved ? bmpPath : "NO (did not reach frame 10)");

    if (!bmpSaved && totalFrames > 0) {
        fprintf(stderr, "  Note: received %llu frames total (< 10), "
                "no BMP saved.\n",
                static_cast<unsigned long long>(totalFrames));
    }

    fprintf(stderr, "============================================\n");

    receiver.Cleanup();
    return 0;
}
