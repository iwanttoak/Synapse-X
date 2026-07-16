#include "PdController.h"
#include "TrtInference.h"
#include "Log.h"

namespace SynapseX {

PdController::PdController() {
    Reset();
}

void PdController::Reset() {
    m_state.prevErrorX = 0;
    m_state.prevErrorY = 0;
    m_state.residualX = 0;
    m_state.residualY = 0;
    m_state.sentWriteIdx = 0;
    m_state.sentCount = 0;
    for (int i = 0; i < 2; ++i) m_state.sentHistory[i] = {0, 0};

    m_isLocked = false;
    m_lockedTargetX = 0;
    m_lockedTargetY = 0;
    m_lostFrames = 0;
    m_lockedPriority = 0;
}

void PdController::SetConfig(const AimConfig& cfg) {
    m_config = cfg;
}

const AimConfig& PdController::GetConfig() const {
    return m_config;
}

std::vector<PdController::AimPoint> PdController::NormalizeDetections(
    const Detection dets[], int numDets,
    const AimConfig& cfg, int screenW, int screenH)
{
    float scrCx = static_cast<float>(screenW) * 0.5f;
    float scrCy = static_cast<float>(screenH) * 0.5f;
    uint8_t modelId = cfg.modelId;

    std::vector<AimPoint> aimPoints;
    aimPoints.reserve(numDets * 2);

    for (int i = 0; i < numDets; ++i) {
        const auto& d = dets[i];
        float bw = d.x2 - d.x1;
        float bh = d.y2 - d.y1;
        float bcx = (d.x1 + d.x2) * 0.5f;
        float bcyCenter = (d.y1 + d.y2) * 0.5f;
        float bcyHead   = d.y1 + bh * cfg.headOffset;

        AimPoint ap;
        ap.cx = bcx;

        switch (modelId) {
        case 0: case 3: case 4:
            if (d.classId == 0 && d.confidence >= cfg.minConfidence) {
                ap.cy       = (cfg.aimPoint == 1) ? bcyHead : bcyCenter;
                ap.priority = 1;
                ap.distance = std::sqrt((bcx-scrCx)*(bcx-scrCx) + (ap.cy-scrCy)*(ap.cy-scrCy));
                aimPoints.push_back(ap);
            }
            break;

        case 1: case 5: case 6:
            if (cfg.aimPoint == 1) {
                if (d.classId == 1 && d.confidence >= cfg.deltaHeadConfidence) {
                    ap.cy       = bcyCenter;
                    ap.priority = 1;
                } else if (d.classId == 0 && d.confidence >= cfg.minConfidence) {
                    ap.cy       = bcyHead;
                    ap.priority = 2;
                } else { break; }
            } else {
                if (d.classId != 0 || d.confidence < cfg.minConfidence) break;
                ap.cy       = bcyCenter;
                ap.priority = 1;
            }
            ap.distance = std::sqrt((bcx-scrCx)*(bcx-scrCx) + (ap.cy-scrCy)*(ap.cy-scrCy));
            aimPoints.push_back(ap);
            break;

        case 2:
            if (d.classId == 0 && d.confidence >= cfg.minConfidence) {
                ap.cy       = (cfg.aimPoint == 1) ? bcyHead : bcyCenter;
                ap.priority = 1;
                ap.distance = std::sqrt((bcx-scrCx)*(bcx-scrCx) + (ap.cy-scrCy)*(ap.cy-scrCy));
                aimPoints.push_back(ap);
            }
            break;

        case 7:
            if (cfg.classFilter != 2 && d.classId != static_cast<uint32_t>(cfg.classFilter))
                break;
            if (d.confidence >= cfg.minConfidence) {
                ap.cy       = (cfg.aimPoint == 1) ? bcyHead : bcyCenter;
                ap.priority = 1;
                ap.distance = std::sqrt((bcx-scrCx)*(bcx-scrCx) + (ap.cy-scrCy)*(ap.cy-scrCy));
                aimPoints.push_back(ap);
            }
            break;

        default:
            if (d.classId == 0 && d.confidence >= cfg.minConfidence) {
                ap.cy       = (cfg.aimPoint == 1) ? bcyHead : bcyCenter;
                ap.priority = 1;
                ap.distance = std::sqrt((bcx-scrCx)*(bcx-scrCx) + (ap.cy-scrCy)*(ap.cy-scrCy));
                aimPoints.push_back(ap);
            }
            break;
        }
    }

    return aimPoints;
}

PdResult PdController::Run(const Detection dets[], int numDets,
                            const AimConfig& cfg, bool aimEnabled,
                            int screenW, int screenH)
{
    PdResult result;

    if (numDets == 0 || !aimEnabled) {
        if (!aimEnabled) Reset();
        return result;
    }

    auto aimPoints = NormalizeDetections(dets, numDets, cfg, screenW, screenH);
    if (aimPoints.empty()) return result;

    float scrCx = static_cast<float>(screenW) * 0.5f;
    float scrCy = static_cast<float>(screenH) * 0.5f;

    // ── 空间锁：目标选择 ──────────────────────────
    const AimPoint* best = nullptr;

    if (m_isLocked) {
        const AimPoint* bestPri1 = nullptr;
        const AimPoint* bestPri2 = nullptr;
        float dPri1 = 1e9f, dPri2 = 1e9f;

        for (const auto& ap : aimPoints) {
            float d = std::sqrt(
                (ap.cx - m_lockedTargetX) * (ap.cx - m_lockedTargetX) +
                (ap.cy - m_lockedTargetY) * (ap.cy - m_lockedTargetY));
            if (d < kKeepLockRadius) {
                if (ap.priority == 1 && d < dPri1) {
                    dPri1 = d; bestPri1 = &ap;
                } else if (ap.priority == 2 && d < dPri2) {
                    dPri2 = d; bestPri2 = &ap;
                }
            }
        }

        if (bestPri1) {
            best = bestPri1;
            m_lockedPriority = 1;
            m_lostFrames = 0;
        } else if (bestPri2) {
            best = bestPri2;
            m_lockedPriority = 2;
            m_lostFrames = 0;
        } else {
            m_lostFrames++;
            if (m_lostFrames > kMaxLostFrames) {
                m_isLocked = false;
                m_lostFrames = 0;
                m_lockedPriority = 0;
            }
        }
    } else {
        const AimPoint* bestPri1 = nullptr;
        const AimPoint* bestPri2 = nullptr;
        float dPri1 = 1e9f, dPri2 = 1e9f;

        for (const auto& ap : aimPoints) {
            if (ap.distance > cfg.aimRange) continue;
            if (ap.priority == 1 && ap.distance < dPri1) {
                dPri1 = ap.distance; bestPri1 = &ap;
            } else if (ap.priority == 2 && ap.distance < dPri2) {
                dPri2 = ap.distance; bestPri2 = &ap;
            }
        }

        if (bestPri1) {
            best = bestPri1;
            m_lockedPriority = 1;
            m_isLocked = true;
            m_lostFrames = 0;
        } else if (bestPri2) {
            best = bestPri2;
            m_lockedPriority = 2;
            m_isLocked = true;
            m_lostFrames = 0;
        }
    }

    if (!best) {
        return result;
    }

    m_lockedTargetX = best->cx;
    m_lockedTargetY = best->cy;
    result.targetX = best->cx;
    result.targetY = best->cy;
    result.distance = best->distance;
    result.lockedPriority = m_lockedPriority;

    float autoScaleX = (screenW > 0)
        ? static_cast<float>(cfg.gameW) / static_cast<float>(screenW) : 1.0f;
    float autoScaleY = (screenH > 0)
        ? static_cast<float>(cfg.gameH) / static_cast<float>(screenH) : 1.0f;

    float dx = (best->cx - scrCx) * autoScaleX;
    float dy = (best->cy - scrCy) * autoScaleY;

    // ── PD 控制器 ────────────────────────────────
    float sumSentX = 0, sumSentY = 0;
    for (int i = 0; i < m_state.sentCount; ++i) {
        sumSentX += static_cast<float>(m_state.sentHistory[i].dx);
        sumSentY += static_cast<float>(m_state.sentHistory[i].dy);
    }
    float realDx = dx - sumSentX;
    float realDy = dy - sumSentY;

    float pixelError = std::sqrt(realDx * realDx + realDy * realDy);
    if (pixelError < kDeadzonePx) return result;
    if (pixelError > cfg.aimRange) return result;

    float currentKp = cfg.Kp + (cfg.kpMax - cfg.Kp) *
        std::exp(-cfg.kpDecay * pixelError);
    if (currentKp > cfg.kpMax) currentKp = cfg.kpMax;
    if (currentKp < cfg.Kp)    currentKp = cfg.Kp;

    float dErrorX = 0, dErrorY = 0;
    dErrorX = realDx - m_state.prevErrorX;
    dErrorY = realDy - m_state.prevErrorY;

    float outX = currentKp * realDx + cfg.Kd * dErrorX;
    float outY = currentKp * realDy + cfg.Kd * dErrorY;

    m_state.residualX += outX;
    m_state.residualY += outY;

    int moveX = 0, moveY = 0;
    if (m_state.residualX >= 1.0f) {
        moveX = static_cast<int>(m_state.residualX);
        m_state.residualX -= static_cast<float>(moveX);
    } else if (m_state.residualX <= -1.0f) {
        moveX = static_cast<int>(m_state.residualX);
        m_state.residualX -= static_cast<float>(moveX);
    }
    if (m_state.residualY >= 1.0f) {
        moveY = static_cast<int>(m_state.residualY);
        m_state.residualY -= static_cast<float>(moveY);
    } else if (m_state.residualY <= -1.0f) {
        moveY = static_cast<int>(m_state.residualY);
        m_state.residualY -= static_cast<float>(moveY);
    }

    m_state.sentHistory[m_state.sentWriteIdx] = {moveX, moveY};
    m_state.sentWriteIdx = (m_state.sentWriteIdx + 1) % 2;
    if (m_state.sentCount < 2) m_state.sentCount++;

    m_state.prevErrorX = realDx;
    m_state.prevErrorY = realDy;

    result.dx = moveX;
    result.dy = moveY;
    result.hasTarget = true;

    return result;
}

} // namespace SynapseX
