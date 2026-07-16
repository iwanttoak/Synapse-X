#pragma once
#include <cstdint>
#include <vector>
#include <cmath>

namespace SynapseX {

struct Detection;  // 来自 TrtInference.h

struct AimConfig {
    float Kp              = 0.30f;
    float Kd              = 0.05f;
    float aimRange        = 200.0f;
    float minConfidence   = 0.25f;
    float deltaHeadConfidence = 0.40f;
    int   aimPoint        = 0;
    float headOffset      = 0.20f;
    int   nativeW         = 3840;
    int   nativeH         = 2160;
    int   gameW           = 3840;
    int   gameH           = 2160;
    uint8_t modelId       = 0;
    float kpMax           = 0.75f;
    float kpDecay         = 0.05f;
    int   classFilter     = 2;
};

struct PdResult {
    int   dx = 0, dy = 0;
    float targetX = 0;
    float targetY = 0;
    float distance = 0;
    bool  hasTarget = false;
    int   lockedPriority = 0;
};

class PdController {
public:
    PdController();

    PdResult Run(const Detection dets[], int numDets,
                 const AimConfig& cfg, bool aimEnabled,
                 int screenW, int screenH);

    void SetConfig(const AimConfig& cfg);
    const AimConfig& GetConfig() const;
    void Reset();

private:
    struct AimPoint {
        float cx, cy;
        int   priority;
        float distance;
    };
    std::vector<AimPoint> NormalizeDetections(
        const Detection dets[], int numDets,
        const AimConfig& cfg, int screenW, int screenH);

    struct PdState {
        float prevErrorX = 0, prevErrorY = 0;
        float residualX = 0, residualY = 0;
        struct SentMove { int dx, dy; };
        SentMove sentHistory[2] = {};
        int sentWriteIdx = 0, sentCount = 0;
    };
    PdState m_state;

    bool  m_isLocked = false;
    float m_lockedTargetX = 0, m_lockedTargetY = 0;
    int   m_lostFrames = 0, m_lockedPriority = 0;

    AimConfig m_config;
    static constexpr float kKeepLockRadius = 80.0f;
    static constexpr int   kMaxLostFrames  = 5;
    static constexpr float kDeadzonePx     = 1.5f;
};

} // namespace SynapseX
