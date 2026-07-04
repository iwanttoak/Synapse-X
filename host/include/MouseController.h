#pragma once

// ── 动态Kp PD控制器（带延迟补偿）──────────────────────────
//
// 核心公式：
//   currentKp = Kp_base + (kpMax − Kp_base) × e^(−kpDecay × pixelError)
//   输出量    = currentKp × realError + Kd × (realError − prevError)
//
//   Kp 从基准值（远距离）自动提升至 kpMax（近距离）。
//   无需基于速度的前馈预测。
//
// 子系统：
//   1. 延迟补偿：从视觉误差中扣除尚未生效的 MoveR 指令。
//   2. 亚像素累加器：仅当 |残差| ≥ 1.0 时发出 MoveR。
//   3. 微死区：误差 < 1.5px 时停止移动。

#include <windows.h>
#include <chrono>
#include <cstdint>
#include <cmath>

namespace SynapseX {

struct AimConfig {
    float Kp              = 0.30f;   // 比例增益
    float Kd              = 0.05f;   // 微分增益（速度阻尼）
    float aimRange        = 200.0f;  // 距屏幕中心最大触发距离（像素）
    float minConfidence       = 0.25f;  // 全局置信度阈值
    float deltaHeadConfidence = 0.40f;  // 头部检测专用置信度阈值
    int   aimPoint            = 0;      // 0 = 身体（中心），1 = 头部（顶部）
    float headOffset      = 0.20f;   // 头部瞄准：边界框顶部比例
    int   nativeW         = 3840;    // 显示器原生宽度
    int   nativeH         = 2160;    // 显示器原生高度
    int   gameW           = 3840;    // 游戏实际宽度（来自网页下拉菜单）
    int   gameH           = 2160;    // 游戏实际高度
    uint8_t modelId       = 0;       // 模型选择器（0=416, 1=640, ...）
    float kpMax           = 0.75f;   // 近距离最大 Kp（磁吸效果）
    float kpDecay         = 0.05f;   // 衰减陡度（值越大，仅在近处才出现磁吸）
};

class MouseController {
public:
    MouseController() = default;
    ~MouseController();

    MouseController(const MouseController&) = delete;
    MouseController& operator=(const MouseController&) = delete;

    // ── DLL 生命周期 ──────────────────────────────────────
    bool Load(const char* dllPath);
    void Unload();
    bool IsLoaded() const { return m_loaded; }

    // ── 底层相对移动 ──────────────────────────────────────
    void MoveRelative(int dx, int dy);

    // ── PD 控制器瞄准（热路径，170 Hz）────────────────────
    //
    // dx, dy = 视觉误差：目标中心 − 屏幕中心（像素）
    // 返回 true 表示已发出鼠标移动指令。
    bool AimAtTarget(float dx, float dy,
                     float confidence,
                     int screenW, int screenH,
                     const AimConfig& cfg = AimConfig{});

    // ── 配置 ──────────────────────────────────────────────
    void SetConfig(const AimConfig& cfg) { m_cfg = cfg; }
    const AimConfig& GetConfig() const { return m_cfg; }

    // ── 状态重置（目标丢失、死区、重新捕获）──────────────
    void ResetPDState();

private:
    HINSTANCE m_dll    = nullptr;
    bool      m_loaded = false;

    using MoveRFn = void (*)(int, int);
    MoveRFn m_moveR = nullptr;

    AimConfig m_cfg;

    // ── PD 状态 ───────────────────────────────────────────
    using Clock     = std::chrono::high_resolution_clock;
    using TimePoint = Clock::time_point;

    float     m_prevErrorX  = 0.0f;
    float     m_prevErrorY  = 0.0f;
    TimePoint m_lastTime;
    bool      m_hasLastTime = false;

    // ── 亚像素累加器 ──────────────────────────────────────
    float     m_residualX   = 0.0f;
    float     m_residualY   = 0.0f;

    // ── 延迟补偿环形缓冲区 ────────────────────────────────
    // 存储最近 N 帧实际发送的 MoveR 值。
    // 通过减去尚未在画面中生效的飞行中移动指令来补偿视觉误差。
    static constexpr int kDelayFrames = 2;
    struct SentMove { int dx; int dy; };
    SentMove  m_sentHistory[kDelayFrames] = {};
    int       m_sentWriteIdx = 0;
    int       m_sentCount    = 0;  // 有效条目数（0..kDelayFrames）
};

} // namespace SynapseX
