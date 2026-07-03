#pragma once

// ── Dynamic-Kp PD Controller with Delay Compensation ──────────
//
// Core formula:
//   currentKp = Kp_base + (kpMax − Kp_base) × e^(−kpDecay × pixelError)
//   Output    = currentKp × realError + Kd × (realError − prevError)
//
//   Kp automatically boosts from base (far-range) to kpMax (point-blank).
//   This eliminates the need for velocity-based feedforward prediction.
//
// Sub-systems:
//   1. Delay Compensation: subtract in-flight MoveR from visual error.
//   2. Sub-pixel Accumulator: emit MoveR only when |residual| ≥ 1.0.
//   3. Micro-deadzone: stop at < 1.5px error.

#include <windows.h>
#include <chrono>
#include <cstdint>
#include <cmath>

namespace SynapseX {

struct AimConfig {
    float Kp              = 0.30f;   // Proportional gain
    float Kd              = 0.05f;   // Derivative gain (velocity damping)
    float aimRange        = 200.0f;  // max px from screen center to engage
    float minConfidence       = 0.25f;  // global confidence filter
    float deltaHeadConfidence = 0.40f;  // Delta head-specific filter
    int   aimPoint            = 0;      // 0 = body (center), 1 = head (top)
    float headOffset      = 0.20f;   // head aim: top fraction of bbox
    int   nativeW         = 3840;    // monitor native width
    int   nativeH         = 2160;    // monitor native height
    int   gameW           = 3840;    // game actual width (from web dropdown)
    int   gameH           = 2160;    // game actual height
    uint8_t modelId       = 0;       // model selector (0=416, 1=640, ...)
    float kpMax           = 0.75f;   // max Kp at point-blank (magnetic snap)
    float kpDecay         = 0.05f;   // decay steepness (higher = snap only near target)
};

class MouseController {
public:
    MouseController() = default;
    ~MouseController();

    MouseController(const MouseController&) = delete;
    MouseController& operator=(const MouseController&) = delete;

    // ── DLL lifecycle ──────────────────────────────────────
    bool Load(const char* dllPath);
    void Unload();
    bool IsLoaded() const { return m_loaded; }

    // ── Low-level relative move ────────────────────────────
    void MoveRelative(int dx, int dy);

    // ── PD-controller aim (hot path, 170 Hz) ───────────────
    //
    // dx, dy = visual error: targetCenter − screenCenter (px)
    // Returns true if a mouse move was emitted.
    bool AimAtTarget(float dx, float dy,
                     float confidence,
                     int screenW, int screenH,
                     const AimConfig& cfg = AimConfig{});

    // ── Config ────────────────────────────────────────────
    void SetConfig(const AimConfig& cfg) { m_cfg = cfg; }
    const AimConfig& GetConfig() const { return m_cfg; }

    // ── State reset (target lost, deadzone, re-acquire) ───
    void ResetPDState();

private:
    HINSTANCE m_dll    = nullptr;
    bool      m_loaded = false;

    using MoveRFn = void (*)(int, int);
    MoveRFn m_moveR = nullptr;

    AimConfig m_cfg;

    // ── PD state ───────────────────────────────────────────
    using Clock     = std::chrono::high_resolution_clock;
    using TimePoint = Clock::time_point;

    float     m_prevErrorX  = 0.0f;
    float     m_prevErrorY  = 0.0f;
    TimePoint m_lastTime;
    bool      m_hasLastTime = false;

    // ── Sub-pixel accumulator ──────────────────────────────
    float     m_residualX   = 0.0f;
    float     m_residualY   = 0.0f;

    // ── Delay compensation ring buffer ─────────────────────
    // Stores the last N frames of actual MoveR values sent.
    // Visual error is compensated by subtracting the sum of
    // in-flight movements that haven't appeared in the frame yet.
    static constexpr int kDelayFrames = 2;
    struct SentMove { int dx; int dy; };
    SentMove  m_sentHistory[kDelayFrames] = {};
    int       m_sentWriteIdx = 0;
    int       m_sentCount    = 0;  // valid entries (0..kDelayFrames)
};

} // namespace SynapseX
