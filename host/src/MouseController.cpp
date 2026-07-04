// ─── MouseController.cpp ─────────────────────────────────────
// 带亚像素累加和延时补偿的 PD 控制器。
//
// 热点路径：AimAtTarget() 在 170 Hz 下运行。零堆分配。

#include "MouseController.h"

#include <cstdio>

namespace SynapseX {

static constexpr float kDeadzonePx   = 1.5f;  // 微死区

// ═══════════════════════════════════════════════════════════════
//  DLL 生命周期
// ═══════════════════════════════════════════════════════════════

MouseController::~MouseController() { Unload(); }

bool MouseController::Load(const char* dllPath) {
    if (m_loaded) Unload();

    m_dll = LoadLibraryA(dllPath);
    if (!m_dll) {
        fprintf(stderr, "[MouseCtrl] LoadLibraryA('%s') 失败 (错误码=%lu)\n",
                dllPath, static_cast<unsigned long>(GetLastError()));
        return false;
    }

    using OpenDeviceFn = int (*)();
    auto openDev = reinterpret_cast<OpenDeviceFn>(
        GetProcAddress(m_dll, "OpenDevice"));
    if (!openDev || openDev() == 0) {
        fprintf(stderr, "[MouseCtrl] OpenDevice 失败。请以管理员身份运行。\n");
        FreeLibrary(m_dll); m_dll = nullptr;
        return false;
    }

    m_moveR = reinterpret_cast<MoveRFn>(GetProcAddress(m_dll, "MoveR"));
    if (!m_moveR) {
        fprintf(stderr, "[MouseCtrl] GetProcAddress('MoveR') 失败\n");
        FreeLibrary(m_dll); m_dll = nullptr;
        return false;
    }

    m_loaded = true;
    ResetPDState();
    fprintf(stderr, "[MouseCtrl] 就绪 -- PD + 亚像素 + 延时补偿 (Kp=%.2f Kd=%.2f)\n",
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
//  状态重置
// ═══════════════════════════════════════════════════════════════

void MouseController::ResetPDState() {
    m_prevErrorX   = 0.0f;
    m_prevErrorY   = 0.0f;
    m_hasLastTime  = false;
    // 清除亚像素累加器
    m_residualX    = 0.0f;
    m_residualY    = 0.0f;
    // 清除延时补偿历史
    m_sentWriteIdx = 0;
    m_sentCount    = 0;
    for (int i = 0; i < kDelayFrames; ++i) m_sentHistory[i] = {0, 0};
}

// ═══════════════════════════════════════════════════════════════
//  PD 控制器瞄准（热点路径）
// ═══════════════════════════════════════════════════════════════

bool MouseController::AimAtTarget(float dx, float dy,
                                   float confidence,
                                   int /*screenW*/, int /*screenH*/,
                                   const AimConfig& cfg) {
    if (!m_loaded) return false;
    if (confidence < cfg.minConfidence) { ResetPDState(); return false; }

    // ── 1. 延时补偿 ─────────────────────────────
    // 从视觉误差中减去已知的飞行中移动量。
    // 这些 MoveR 调用已发送但尚未出现在采集管线中
    // （1-2 帧的视觉延迟）。
    float sumSentX = 0.0f, sumSentY = 0.0f;
    for (int i = 0; i < m_sentCount; ++i) {
        sumSentX += static_cast<float>(m_sentHistory[i].dx);
        sumSentY += static_cast<float>(m_sentHistory[i].dy);
    }
    float realDx = dx - sumSentX;
    float realDy = dy - sumSentY;

    // ── 2. 微死区 ──────────────────────────────────
    float pixelError = std::sqrt(realDx * realDx + realDy * realDy);
    if (pixelError < 1.5f) {
        ResetPDState();
        return false;
    }

    // 距离门限
    if (pixelError > cfg.aimRange) {
        ResetPDState();
        return false;
    }

    // ── 3. 动态 Kp（指数衰减到基准值）─────────────
    // Kp_actual = Kp_base + (Kp_max - Kp_base) * exp(-k * Error)
    //   远距离：exp(-∞) ≈ 0  → Kp = Kp_base（温和追踪）
    //   近距离：exp(0) = 1  → Kp = Kp_max（磁吸效果）
    float currentKp = cfg.Kp + (cfg.kpMax - cfg.Kp) *
        std::exp(-cfg.kpDecay * pixelError);
    if (currentKp > cfg.kpMax) currentKp = cfg.kpMax;
    if (currentKp < cfg.Kp)     currentKp = cfg.Kp;

    // ── 4. D 项（对补偿后误差增量的阻尼）────
    float dErrorX = 0.0f, dErrorY = 0.0f;
    if (m_hasLastTime) {
        dErrorX = realDx - m_prevErrorX;
        dErrorY = realDy - m_prevErrorY;
    }

    // ── 5. PD 输出 ───────────────────────────────────
    float outX = currentKp * realDx + cfg.Kd * dErrorX;
    float outY = currentKp * realDy + cfg.Kd * dErrorY;

    // ── 6. 亚像素累加器 ──────────────────────────
    // 累加分数部分输出。仅在 |残差| ≥ 1.0 时发出 MoveR，
    // 然后提取并减去整数部分。
    // 这产生完美平滑的追踪 -- 没有强制的 ±1 抖动，
    // 没有死区边缘的量化伪影。
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

    // ── 7. 执行 ─────────────────────────────────────────
    if (moveX != 0 || moveY != 0) {
        MoveRelative(moveX, moveY);
    }

    // ── 8. 记录到延时环形缓冲区（用于未来补偿）──
    m_sentHistory[m_sentWriteIdx] = {moveX, moveY};
    m_sentWriteIdx = (m_sentWriteIdx + 1) % kDelayFrames;
    if (m_sentCount < kDelayFrames) m_sentCount++;

    // ── 9. 保存状态 ─────────────────────────────────────
    m_prevErrorX  = realDx;
    m_prevErrorY  = realDy;
    m_hasLastTime = true;

    return true;
}

} // namespace SynapseX
