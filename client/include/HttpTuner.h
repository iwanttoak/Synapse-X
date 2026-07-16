#pragma once

#include "PdController.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace SynapseX {

struct TuningState {
    AimConfig config;
    bool      aimEnabled = true;

    // 副机统计
    double receiveFps   = 0.0;
    double inferFps     = 0.0;
    double inferMs      = 0.0;
    double pipelineMs   = 0.0;
    int    freshFrames  = 0;
    int    droppedFrames = 0;

    // 目标信息
    struct TargetInfo {
        bool   active      = false;
        float  screenX     = 0.0f;
        float  screenY     = 0.0f;
        float  confidence  = 0.0f;
        float  distance    = 0.0f;
        int    dx          = 0;
        int    dy          = 0;
    };
    TargetInfo target;

    // 热键状态（来自 PacketHeader.flags）
    bool hostAimEnabled = true;

    // 服务器信息
    int     serverPort  = 9999;
    bool    running     = false;
};

class HttpTuner {
public:
    HttpTuner() = default;
    ~HttpTuner();

    HttpTuner(const HttpTuner&) = delete;
    HttpTuner& operator=(const HttpTuner&) = delete;

    bool Start(uint16_t port = 9999);
    void Stop();
    bool IsRunning() const { return m_state.running; }

    AimConfig GetConfig() const;
    bool      IsAimEnabled() const;
    void      SetAimEnabled(bool enabled);

    void UpdateStats(double receiveFps, double inferFps,
                     double inferMs, double pipelineMs,
                     int fresh, int dropped);

    void UpdateTarget(float screenX, float screenY,
                      float confidence, float distance,
                      int dx, int dy);

    void SetHostAimEnabled(bool enabled);

private:
    void ServerThread();

    TuningState m_state;
    mutable std::mutex m_mutex;
    std::thread m_thread;
};

} // namespace SynapseX
