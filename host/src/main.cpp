// ─── Synapse-X 主机端 -- 生产主循环 ─────────────────
//
// 固定的 170 Hz 管线：
//   DXGI 采集（每个 tick 尝试）
//     -> 新帧？LZ4 压缩 + 更新缓存
//     -> 无变化？重新发送缓存的压缩帧
//   UDP 分片与发送（每个 tick，携带 flags=热键状态）
//
// 用法：
//   SynapseX_Host.exe [目标IP] [端口] [roi宽] [roi高] [modelId]
//   默认值：192.168.100.2  8888   416     416     0

#include "DxgiCapturer.h"
#include "Log.h"
#include "Lz4Compressor.h"
#include "UdpSender.h"
#include "PacketHeader.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <vector>
#include <atomic>
#include <thread>

#include <windows.h>
#include <mmsystem.h>

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
        captured = 0; sent = 0;
        sumCapture = 0.0; sumCompress = 0.0; sumSend = 0.0;
    }
};

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SynapseX::Log::Initialize("host");

    const char* targetIp   = (argc > 1) ? argv[1] : "192.168.100.2";
    uint16_t    targetPort = (argc > 2) ? static_cast<uint16_t>(std::atoi(argv[2])) : 8888;
    int         roiW       = (argc > 3) ? std::atoi(argv[3]) : 416;
    int         roiH       = (argc > 4) ? std::atoi(argv[4]) : 416;
    uint8_t     modelId    = (argc > 5) ? static_cast<uint8_t>(std::atoi(argv[5])) : 0;

    if (roiW < 64 || roiH < 64 || roiW > 4096 || roiH > 4096) {
        SX_LOG_CRITICAL("[Host] 无效的 ROI {}x{}（允许范围: 64..4096）", roiW, roiH);
        return 1;
    }

    int rawSize = roiW * roiH * 4;

    constexpr double kTargetFps  = 170.0;
    constexpr double kTargetMs   = 1000.0 / kTargetFps;
    const auto       kInterval   = std::chrono::duration<double, std::milli>(kTargetMs);

    timeBeginPeriod(1);

    DWORD_PTR affinityMask = 1ULL << 2;
    if (!SetThreadAffinityMask(GetCurrentThread(), affinityMask)) {
        SX_LOG_WARN("[Host] SetThreadAffinityMask 失败 (error={})",
                    static_cast<unsigned long>(GetLastError()));
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    SX_LOG_INFO("[Host] 主线程已绑定到核心2，TIME_CRITICAL 优先级");
    SX_LOG_INFO("[Host] 启动固定频率管线: target={}Hz target={}:{} roi={}x{} raw_mb={:.2f} modelId={}",
                kTargetFps, targetIp, targetPort, roiW, roiH,
                rawSize / (1024.0 * 1024.0), modelId);

    SynapseX::DxgiCapturer capturer;
    if (!capturer.Initialize(roiW, roiH)) {
        SX_LOG_CRITICAL("[Host] DxgiCapturer 初始化失败");
        return 1;
    }

    SynapseX::Lz4Compressor compressor;
    if (!compressor.Initialize(rawSize)) {
        SX_LOG_CRITICAL("[Host] Lz4Compressor 初始化失败");
        return 1;
    }

    SynapseX::UdpSender sender;
    if (!sender.Initialize(targetIp, targetPort)) {
        SX_LOG_CRITICAL("[Host] UdpSender 初始化失败");
        return 1;
    }

    int screenW = capturer.GetOutputWidth();
    int screenH = capturer.GetOutputHeight();

    SX_LOG_INFO("[Host] 所有模块已初始化；进入主循环");
    SX_LOG_INFO("[Host] 按 Ctrl+C 停止");

    int      zeroFrameCount = 0;
    bool     warnedProtected = false;
    bool     warnedZero = false;

    std::vector<uint8_t> rawBuffer;
    rawBuffer.reserve(rawSize);

    std::vector<uint8_t> compressedBuffer;
    compressedBuffer.reserve(SynapseX::Lz4Compressor::GetMaxOutputSize(rawSize));

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

    while (g_running) {
        // ── 热键：PageUp=开启瞄准, PageDown=关闭瞄准 ──────────
        static bool pgupWasDown = false, pgdnWasDown = false;
        bool pgupDown = (GetAsyncKeyState(VK_PRIOR) & 0x8000) != 0;
        bool pgdnDown = (GetAsyncKeyState(VK_NEXT)  & 0x8000) != 0;

        uint8_t flags = 0;
        bool aimEnabled = false;
        if (pgupDown && !pgupWasDown) {
            aimEnabled = true;
            SX_LOG_INFO("[Host] 自瞄已通过热键启用");
        } else if (pgdnDown && !pgdnWasDown) {
            SX_LOG_INFO("[Host] 自瞄已通过热键禁用");
        }
        if (pgupDown) aimEnabled = true;
        if (pgdnDown) aimEnabled = false;
        if (aimEnabled) flags |= SynapseX::FF_AIM_ENABLED;
        pgupWasDown = pgupDown;
        pgdnWasDown = pgdnDown;

        // ── 阶段 1：采集 ──────────────────────────────
        auto t0 = Clock::now();
        bool gotFrame = capturer.CaptureFrame(rawBuffer);
        auto t1 = Clock::now();

        if (gotFrame) {
            const auto& fi = capturer.GetLastFrameInfo();
            if (fi.ProtectedContentMaskedOut && !warnedProtected) {
                SX_LOG_WARN("[Host] 检测到受保护内容已屏蔽；请尝试以无边框窗口模式运行游戏");
                warnedProtected = true;
            }

            bool allZero = true;
            for (size_t k = 0; k < rawBuffer.size(); ++k) {
                if (rawBuffer[k] != 0) { allZero = false; break; }
            }
            if (allZero) {
                zeroFrameCount++;
                if (!warnedZero && zeroFrameCount > 10) {
                    SX_LOG_WARN("[Host] 检测到连续{}个零帧：屏幕={}x{} ROI={}x{} 受保护={}",
                                zeroFrameCount, screenW, screenH, roiW, roiH,
                                fi.ProtectedContentMaskedOut);
                    warnedZero = true;
                }
            } else {
                zeroFrameCount = 0;
                warnedZero = false;
            }

            auto t2a = Clock::now();
            bool ok = compressor.Compress(rawBuffer.data(),
                                           static_cast<int>(rawBuffer.size()),
                                           compressedBuffer);
            auto t2b = Clock::now();

            if (ok) {
                cachedCompressed = compressedBuffer;
                hasCachedFrame   = true;
                stats.captured++;
                stats.sumCapture  += ToMs(t1 - t0);
                stats.sumCompress += ToMs(t2b - t2a);
            }
        }

        // ── 阶段 2：发送 ──────────────────────────────
        if (hasCachedFrame) {
            auto t3a = Clock::now();
            bool sent = sender.SendCompressedFrame(
                cachedCompressed.data(),
                static_cast<uint32_t>(cachedCompressed.size()),
                frameId,
                roiW16, roiH16,
                modelId,
                flags);
            auto t3b = Clock::now();

            if (sent) totalSent++;
            stats.sent++;
            stats.sumSend += ToMs(t3b - t3a);
            frameId++;
        }

        // ── 每秒统计报告 ──────────────────────────
        double elapsed = ToMs(Clock::now() - windowStart) / 1000.0;
        if (elapsed >= 1.0) {
            double sendFps    = stats.sent / elapsed;
            double captureFps = stats.captured / elapsed;
            double avgCapture = stats.captured > 0 ? stats.sumCapture  / stats.captured : 0.0;
            double avgCompress= stats.captured > 0 ? stats.sumCompress / stats.captured : 0.0;
            double avgSend    = stats.sent     > 0 ? stats.sumSend     / stats.sent     : 0.0;

            SX_LOG_DEBUG("[Host] 统计：发送帧率={:.1f} 采集帧率={:.1f} 新帧数={} 缓存帧数={} 总发送数={} 采集={:.3f}ms 压缩={:.3f}ms 发送={:.3f}ms",
                         sendFps, captureFps, stats.captured,
                         stats.sent - stats.captured,
                         static_cast<long long>(totalSent),
                         avgCapture, avgCompress, avgSend);

            stats.reset();
            windowStart = Clock::now();
        }

        // ── 维持 170 Hz 节奏 ──────────────────────────
        nextTick += std::chrono::duration_cast<Clock::duration>(kInterval);
        auto now = Clock::now();
        if (nextTick > now) {
            std::this_thread::sleep_until(nextTick);
        } else {
            nextTick = now;
        }
    }

    timeEndPeriod(1);

    double sessionSec = ToMs(Clock::now() - sessionStart) / 1000.0;
    SX_LOG_INFO("[Host] 会话已结束：时长={:.1f}秒 总发送数={} 平均帧率={:.1f}",
                sessionSec,
                static_cast<long long>(totalSent),
                sessionSec > 0.0 ? totalSent / sessionSec : 0.0);

    capturer.Cleanup();
    sender.Cleanup();
    return 0;
}
