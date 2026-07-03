// ─── MouseController.cpp ─────────────────────────────────────
// PD-controller with sub-pixel accumulation and delay compensation.
//
// Hot path: AimAtTarget() at 170 Hz. Zero heap allocation.

#include "MouseController.h"

#include <cstdio>

namespace SynapseX {

static constexpr float kDeadzonePx   = 1.5f;  // micro-deadzone

// ═══════════════════════════════════════════════════════════════
//  DLL lifecycle
// ═══════════════════════════════════════════════════════════════

MouseController::~MouseController() { Unload(); }

bool MouseController::Load(const char* dllPath) {
    if (m_loaded) Unload();

    m_dll = LoadLibraryA(dllPath);
    if (!m_dll) {
        fprintf(stderr, "[MouseCtrl] LoadLibraryA('%s') FAILED (err=%lu)\n",
                dllPath, static_cast<unsigned long>(GetLastError()));
        return false;
    }

    using OpenDeviceFn = int (*)();
    auto openDev = reinterpret_cast<OpenDeviceFn>(
        GetProcAddress(m_dll, "OpenDevice"));
    if (!openDev || openDev() == 0) {
        fprintf(stderr, "[MouseCtrl] OpenDevice FAILED. Run as Administrator.\n");
        FreeLibrary(m_dll); m_dll = nullptr;
        return false;
    }

    m_moveR = reinterpret_cast<MoveRFn>(GetProcAddress(m_dll, "MoveR"));
    if (!m_moveR) {
        fprintf(stderr, "[MouseCtrl] GetProcAddress('MoveR') FAILED\n");
        FreeLibrary(m_dll); m_dll = nullptr;
        return false;
    }

    m_loaded = true;
    ResetPDState();
    fprintf(stderr, "[MouseCtrl] Ready — PD + sub-pixel + delay-comp (Kp=%.2f Kd=%.2f)\n",
            static_cast<double>(m_cfg.Kp), static_cast<double>(m_cfg.Kd));
    return true;
}

void MouseController::Unload() {
    if (m_dll) { FreeLibrary(m_dll); m_dll = nullptr; m_moveR = nullptr; m_loaded = false; }
}

void MouseController::MoveRelative(int dx, int dy) {
    if (m_moveR) m_moveR(dx, dy);
}

// ═══════════════════════════════════════════════════════════════
//  State reset
// ═══════════════════════════════════════════════════════════════

void MouseController::ResetPDState() {
    m_prevErrorX   = 0.0f;
    m_prevErrorY   = 0.0f;
    m_hasLastTime  = false;
    // Clear sub-pixel accumulators
    m_residualX    = 0.0f;
    m_residualY    = 0.0f;
    // Clear delay compensation history
    m_sentWriteIdx = 0;
    m_sentCount    = 0;
    for (int i = 0; i < kDelayFrames; ++i) m_sentHistory[i] = {0, 0};
}

// ═══════════════════════════════════════════════════════════════
//  PD-controller aim (hot path)
// ═══════════════════════════════════════════════════════════════

bool MouseController::AimAtTarget(float dx, float dy,
                                   float confidence,
                                   int /*screenW*/, int /*screenH*/,
                                   const AimConfig& cfg) {
    if (!m_loaded) return false;
    if (confidence < cfg.minConfidence) { ResetPDState(); return false; }

    // ── 1. Delay compensation ─────────────────────────────
    // Subtract known in-flight moves from visual error.
    // These MoveR calls were already sent but haven't appeared
    // in the capture pipeline yet (1–2 frame visual latency).
    float sumSentX = 0.0f, sumSentY = 0.0f;
    for (int i = 0; i < m_sentCount; ++i) {
        sumSentX += static_cast<float>(m_sentHistory[i].dx);
        sumSentY += static_cast<float>(m_sentHistory[i].dy);
    }
    float realDx = dx - sumSentX;
    float realDy = dy - sumSentY;

    // ── 2. Micro-deadzone ──────────────────────────────────
    float pixelError = std::sqrt(realDx * realDx + realDy * realDy);
    if (pixelError < 1.5f) {
        ResetPDState();
        return false;
    }

    // Range gate
    if (pixelError > cfg.aimRange) {
        ResetPDState();
        return false;
    }

    // ── 3. Dynamic Kp (exponential decay toward base) ──────
    // Kp_actual = Kp_base + (Kp_max - Kp_base) * exp(-k * Error)
    //   Far away:  exp(-∞) ≈ 0  → Kp = Kp_base (gentle tracking)
    //   Point blank: exp(0) = 1 → Kp = Kp_max  (magnetic snap)
    float currentKp = cfg.Kp + (cfg.kpMax - cfg.Kp) *
        std::exp(-cfg.kpDecay * pixelError);
    if (currentKp > cfg.kpMax) currentKp = cfg.kpMax;
    if (currentKp < cfg.Kp)     currentKp = cfg.Kp;

    // ── 4. D-term (damping on compensated error delta) ─────
    float dErrorX = 0.0f, dErrorY = 0.0f;
    if (m_hasLastTime) {
        dErrorX = realDx - m_prevErrorX;
        dErrorY = realDy - m_prevErrorY;
    }

    // ── 5. PD output ───────────────────────────────────────
    float outX = currentKp * realDx + cfg.Kd * dErrorX;
    float outY = currentKp * realDy + cfg.Kd * dErrorY;

    // ── 6. Sub-pixel accumulator ──────────────────────────
    // Accumulate fractional output. Only emit MoveR when
    // |residual| ≥ 1.0, then extract and subtract the integer part.
    // This produces perfectly smooth tracking — no forced ±1 jitter,
    // no quantization artifacts at the deadzone edge.
    m_residualX += outX;
    m_residualY += outY;

    int moveX = 0, moveY = 0;

    if (m_residualX >= 1.0f) {
        moveX = static_cast<int>(m_residualX);
        m_residualX -= static_cast<float>(moveX);
    } else if (m_residualX <= -1.0f) {
        moveX = static_cast<int>(m_residualX);
        m_residualX -= static_cast<float>(moveX);
    }

    if (m_residualY >= 1.0f) {
        moveY = static_cast<int>(m_residualY);
        m_residualY -= static_cast<float>(moveY);
    } else if (m_residualY <= -1.0f) {
        moveY = static_cast<int>(m_residualY);
        m_residualY -= static_cast<float>(moveY);
    }

    // ── 7. Execute ─────────────────────────────────────────
    if (moveX != 0 || moveY != 0) {
        MoveRelative(moveX, moveY);
    }

    // ── 8. Record in delay ring (for future compensation) ──
    m_sentHistory[m_sentWriteIdx] = {moveX, moveY};
    m_sentWriteIdx = (m_sentWriteIdx + 1) % kDelayFrames;
    if (m_sentCount < kDelayFrames) m_sentCount++;

    // ── 9. Save state ─────────────────────────────────────
    m_prevErrorX  = realDx;
    m_prevErrorY  = realDy;
    m_hasLastTime = true;

    return true;
}

} // namespace SynapseX
