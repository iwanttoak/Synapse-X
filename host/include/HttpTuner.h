#pragma once

// ── 网页调参面板 ────────────────────────────────────────────
// 在 localhost:9999 上运行轻量级 HTTP 服务器。
// 局域网内任何设备（手机、平板、笔记本）均可打开
// http://192.168.100.1:9999 实时调节瞄准参数。
//
// API 接口：
//   GET  /            → 内嵌 HTML 控制面板
//   GET  /api/state   → JSON：当前配置 + 流水线统计数据
//   POST /api/config  → JSON 体：更新 AimConfig 字段
//
// 依赖：cpp-httplib（单头文件库，位于 thirdparty/ 目录）

#include "MouseController.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace SynapseX {

struct TuningState {
    // ── 瞄准参数（可通过 Web UI 修改）─────────────────────
    AimConfig config;
    bool      aimEnabled = true;

    // ── 流水线统计（由主循环写入，Web UI 读取）────────────
    double sendFps       = 0.0;
    double captureFps    = 0.0;
    double pipelineMs    = 0.0;   // 总流水线延迟
    double compressMs    = 0.0;
    int    freshFrames   = 0;
    int    cacheFrames   = 0;
    uint64_t totalSent   = 0;

    // ── 最新检测信息 ──────────────────────────────────────
    struct TargetInfo {
        bool   active      = false;
        float  screenX     = 0.0f;
        float  screenY     = 0.0f;
        float  confidence  = 0.0f;
        float  distance    = 0.0f;
        int    classId     = 0;
    };
    TargetInfo target;

    // ── 服务器信息 ────────────────────────────────────────
    int     serverPort  = 9999;
    bool    running     = false;
};

class HttpTuner {
public:
    HttpTuner() = default;
    ~HttpTuner();

    HttpTuner(const HttpTuner&) = delete;
    HttpTuner& operator=(const HttpTuner&) = delete;

    // 在后台线程中启动 HTTP 服务器。
    // 端口：默认 9999。为安全起见仅绑定到 localhost。
    bool Start(uint16_t port = 9999);

    // 停止服务器并等待线程结束。
    void Stop();

    bool IsRunning() const { return m_state.running; }

    // ── 主循环线程安全访问接口 ────────────────────────────

    // 读取当前配置（用于瞄准辅助）
    AimConfig GetConfig() const;
    bool      IsAimEnabled() const;
    void      SetAimEnabled(bool enabled);

    // 更新流水线统计（每秒调用一次）
    void UpdateStats(double sendFps, double captureFps,
                     double pipelineMs, double compressMs,
                     int fresh, int cache, uint64_t totalSent);

    // 更新最新目标信息
    void UpdateTarget(float screenX, float screenY,
                      float confidence, float distance, int classId);

private:
    void ServerThread();

    TuningState m_state;
    mutable std::mutex m_mutex;
    std::thread m_thread;
};

} // namespace SynapseX
