# Mouse Control Specification

> Complete specification of the Host-side aim-assist pipeline.
> Covers: target selection, aim point computation, PD controller, deadzones,
> DLL integration, web tuning parameters, and all configurable constants.

---

## 1. Architecture Overview

```
main.cpp (170 Hz tick)
  │
  ├── Receive replies from UdpReplyReceiver
  │     └── Detection[] (screen-space, classId, confidence)
  │
  ├── Target Selection
  │     ├── Filter: enemy only (classId == 0)
  │     ├── Sort: highest confidence, tie-break by closest to screen center
  │     └── Output: best Detection*
  │
  ├── Aim Point Computation
  │     ├── aimPoint=0 (Body): target = bbox center → (x1+x2)/2, (y1+y2)/2
  │     └── aimPoint=1 (Head): target = bbox top   → (x1+x2)/2, y1 + bboxH * headOffset
  │
  ├── Error Vector
  │     ├── dx = targetCx - screenW/2
  │     ├── dy = targetCy - screenH/2
  │     └── pass (dx, dy) to MouseController::AimAtTarget()
  │
  └── MouseController (PD controller)
        ├── Deadzone check
        ├── Range gate
        ├── PD compute: Output = Kp*error + Kd*dE
        ├── Quantize + min-step forcing
        └── ddll64.dll MoveR(moveX, moveY)
```

### Coordinate spaces

| Space | Origin | Unit | Who produces it |
|-------|--------|------|-----------------|
| Model pixel | Top-left of ROI | px (0–415) | Client (TRT inference) |
| Screen | Top-left of full display | px (0–3839) | `UdpReplyReceiver` maps model→screen via `screen = roiOffset + model` |
| Aim error | Screen center | px offset | `main.cpp`: `dx = target - screenCenter` |

---

## 2. Target Selection (main.cpp)

### Algorithm

```
Input:  std::vector<Detection> — all detections from current Client reply
Output: const Detection* best  — or nullptr if no valid target

1. For each detection d:
     if d.classId != 0: skip        // enemy only
     cx = (d.x1 + d.x2) / 2         // bbox center
     cy = (d.y1 + d.y2) / 2
     dist = sqrt((cx-scrCx)² + (cy-scrCy)²)

2. Pick best:
     if no best yet: pick this one
     elif d.confidence > best.confidence: pick this one
     elif d.confidence == best.confidence && dist < bestDist: pick this one
```

### Notes

- **Single-target lock**: Only one target per frame. No multi-target averaging.
- **No temporal stickiness**: Switches targets instantly if confidence changes.
  (Known issue A3 — future fix: require N frames before switching.)
- **Teammates (classId=1) are ignored**: Only enemy targets trigger aim.

---

## 3. Aim Point Computation (main.cpp)

Determines WHERE on the target bbox to aim.

```
float bboxH = best->y2 - best->y1;

targetCx = (best->x1 + best->x2) * 0.5f;  // always horizontal center

if (aimPoint == 1):  // Head
    targetCy = best->y1 + bboxH * headOffset;
else:                // Body (default)
    targetCy = (best->y1 + best->y2) * 0.5f;
```

### Parameters

| Parameter | Type | Default | Range | Description |
|-----------|------|---------|-------|-------------|
| `aimPoint` | int | 0 | 0 or 1 | 0=body center, 1=head |
| `headOffset` | float | 0.12 | 0.05–0.25 | Fraction from top of bbox (head position) |

### Visual

```
┌─────────────┐ ← y1
│   headOff=12%│ ← aimPoint=1: target here (top 12%)
│             │
│     × center│ ← aimPoint=0: target here (geometric center)
│             │
└─────────────┘ ← y2 = y1 + bboxH
```

---

## 4. Error Vector (main.cpp)

Screen-center offset passed to PD controller:

```
float dx = targetCx - screenW * 0.5f;
float dy = targetCy - screenH * 0.5f;
```

- `dx > 0` → target is to the right of crosshair
- `dy > 0` → target is below crosshair
- Both are in **physical screen pixels** (not normalized, not model-space)

---

## 5. PD Controller (MouseController::AimAtTarget)

### 5.1 Flowchart

```
AimAtTarget(dx, dy, confidence, cfg)   ← dx,dy = visual error
  │
  ├─ Guard: !m_loaded → return false
  ├─ Guard: confidence < cfg.minConfidence → reset ALL state, return false
  │
  ├─ STEP 1: Delay compensation
  │    sumSentX/Y = sum of last 2 frames' MoveR values from ring buffer
  │    realDx = dx - sumSentX
  │    realDy = dy - sumSentY
  │
  ├─ STEP 2: Micro-deadzone (on compensated error)
  │    pixelError = sqrt(realDx² + realDy²)
  │    if pixelError < 1.5 px → reset ALL state, return false
  │
  ├─ STEP 3: Range gate
  │    if pixelError > cfg.aimRange → reset ALL state, return false
  │
  ├─ STEP 4: Dynamic Kp (exponential decay)
  │    currentKp = Kp_base + (kpMax − Kp_base) × e^(−kpDecay × pixelError)
  │    clamp to [Kp, kpMax]
  │
  ├─ STEP 5: D-term (on compensated error delta)
  │    dError = realError − prevError (only if hasPrevError)
  │    D = Kd × dError
  │
  ├─ STEP 6: PD output
  │    Output = currentKp × realError + Kd × dError
  │
  ├─ STEP 7: Sub-pixel accumulator
  │    residualX += OutputX
  │    residualY += OutputY
  │    if |residualX| ≥ 1.0: moveX = int(residualX); residualX -= moveX
  │    same for Y
  │
  ├─ STEP 8: Execute
  │    if moveX != 0 || moveY != 0: MoveR(moveX, moveY)
  │
  ├─ STEP 9: Record in delay ring
  │    sentHistory[writeIdx] = {moveX, moveY}
  │    writeIdx = (writeIdx + 1) % 2
  │
  └─ STEP 10: Save PD state
       prevErrorX = realDx, prevErrorY = realDy
       hasPrevError = true
```

### 5.2 Dynamic Kp Formula

```
pixelError = √(realDx² + realDy²)

Kp_actual = Kp_base + (kpMax − Kp_base) × e^(−kpDecay × pixelError)

                                          ←──── ──── →  ← 远距离用 base Kp，贴脸 boost 到 kpMax

Example (Kp=0.30, kpMax=0.75, kpDecay=0.05):
  200px → Kp≈0.30  (gentle tracking)
   50px → Kp≈0.38  (engaging)
   10px → Kp≈0.57  (magnetic)
    0px → Kp=0.75  (max snap)

PD output per axis:
  Output = currentKp × realError + Kd × (realError − prevError)

  residual += Output                                   ← sub-pixel accumulator
  if |residual| ≥ 1.0: emit MoveR(int(residual)), subtract int part
```

**Why no dt division**: At fixed 170Hz, `dt ≈ 0.00588s` is effectively constant.
Dividing by dt would amplify derivative noise by 170×. Direct `dE` (pixels per
frame) gives intuitive Kd tuning.

**Why no I-term**: Integral would accumulate past errors, causing wind-up on
target switch and overshoot at close range. At 170Hz, P+D alone is sufficient.

**Why no dE deadband**: The old `|dE| < 0.5` gate blocked slow-tracking
movement. The sub-pixel accumulator now handles this naturally — sub-pixel
output accumulates frame-by-frame until a full pixel move is emitted, with
no hard cutoff interfering.

### 5.3 Delay Compensation

In FPS games, `MoveR(dx,dy)` rotates the camera angle, not the cursor position.
The visual pipeline has 1–2 frames of latency (capture→network→inference→reply→aim).
The PD controller would see "old" error that doesn't yet reflect recently-sent movements,
causing it to re-apply thrust and overshoot.

**Solution**: Before PD computation, subtract the sum of the last 2 frames' sent
`MoveR` values from the visual error:

```
realDx = dx − sum(sentMoveX[0..kDelayFrames-1])
realDy = dy − sum(sentMoveY[0..kDelayFrames-1])
```

`realDx/realDy` is then fed into the PD controller. This tells the controller:
"Here's the error AFTER accounting for movements already in flight."

A ring buffer (`m_sentHistory[2]`) stores the actual (moveX, moveY) values
emitted to `MoveR` in the last 2 frames. On state reset (target lost, deadzone,
re-acquire), the buffer is zeroed — old movements in a different direction
must not pollute the new target's error.

### 5.4 Sub-pixel Accumulator

Replaces both the old forced-±1 quantization and the dE deadband.

```
m_residualX += outputX;
m_residualY += outputY;

int moveX = 0, moveY = 0;

if (m_residualX ≥ 1.0f) {
    moveX = (int)m_residualX;        // e.g. 1.7 → 1
    m_residualX -= (float)moveX;     // keeps 0.7 for next frame
} else if (m_residualX ≤ −1.0f) {
    moveX = (int)m_residualX;
    m_residualX -= (float)moveX;
}
// same for Y axis
```

This acts like a water bucket — fractional output trickles in each frame.
When the bucket fills to ≥1 pixel, a `MoveR` is emitted and that pixel is
"spent". The remaining fraction carries over to the next frame.

**Why this eliminates jitter**:
- No forced ±1 at the deadzone edge — if output is 0.3, nothing happens until
  it naturally accumulates past 1.0
- Sub-pixel tracking is perfectly smooth — the crosshair can move 1px every
  2–3 frames without any quantization hammering
- The dE deadband is no longer needed — legitimate slow-tracking changes
  accumulate naturally through the residual

### 5.5 Noise Gates

| Layer | Type | Threshold | Effect |
|-------|------|-----------|--------|
| Micro-deadzone | Spatial | `pixelError < 1.5px` | Stop ALL movement near target. |
| Range gate | Spatial | `pixelError > aimRange` | Don't engage beyond FOV radius. |
| Confidence gate | Detection | `conf < minConfidence` | Discard low-confidence detections. |

The old 3px deadzone, dt tracking, and derivative deadband have been removed.
The dynamic Kp naturally decays to base Kp at long range (no extra damping needed).

### 5.5 State Reset Triggers

| Trigger | Effect |
|---------|--------|
| `confidence < minConfidence` | Reset (prevError=0, no lastTime) |
| `dist < deadzone (3px)` | Reset — target acquired, no movement needed |
| `dist > aimRange` | Reset — out of range |
| `dt > 50ms` (frame gap) | Force prevError = current error (prevent D spike) |
| First call after load | Force prevError = current error |

Resetting clears `prevErrorX/Y` and `m_hasLastTime`, so the next call treats the error as the initial state — no derivative spike.

---

## 6. Configurable Parameters (Web Panel)

```cpp
struct AimConfig {
    float Kp              = 0.40f;   // Proportional gain
    float Kd              = 0.05f;   // Derivative gain (velocity damping)
    float aimRange        = 500.0f;  // Max px from center to engage
    float minConfidence   = 0.25f;   // Ignore below this
    int   aimPoint        = 0;       // 0=body, 1=head
    float headOffset      = 0.12f;   // Head aim: top fraction of bbox
};
```

| Parameter | Web Slider Range | Step | Role |
|-----------|-----------------|------|------|
| `Kp` | 0.05 – 1.50 | 0.01 | **Base Kp** (far-range tracking). Higher = faster flick. |
| `Kd` | 0.00 – 0.50 | 0.01 | Braking force. Higher = less overshoot. 0 = pure P. |
| `kpMax` | 0.10 – 1.50 | 0.05 | Max Kp at point-blank. Magnetic snap ceiling. |
| `kpDecay` | 0.00 – 0.15 | 0.01 | Decay steepness. Higher = snap only at very close range. |
| `aimRange` | 50 – 1000 | 10 | Engagement radius in pixels. |
| `minConfidence` | 0.00 – 1.00 | 0.01 | Minimum detection confidence to engage. |
| `aimPoint` | 0 / 1 | — | Body center or head. |
| `headOffset` | 0.05 – 0.25 | 0.01 | Head position as fraction from bbox top. |

### Tuning Guide

```
Step 1: Kd=0, kpMax=Kp. Tune Kp (base) for smooth long-range tracking.
Step 2: Raise kpMax until close-range snap feels magnetic but not jittery.
Step 3: Tune kpDecay — lower = snap activates further out, higher = only at point-blank.
Step 4: Add Kd in 0.01 increments to dampen close-range overshoot.
Step 5: aimRange + minConfidence as before.

Typical profiles:
  Smooth tracking:    Kp=0.25  kpMax=0.50  kpDecay=0.03  Kd=0.05
  Balanced (default): Kp=0.30  kpMax=0.75  kpDecay=0.05  Kd=0.05
  Aggressive snap:    Kp=0.40  kpMax=1.20  kpDecay=0.08  Kd=0.10
```

### Removed Parameters

| Parameter | Why removed |
|-----------|-------------|
| `smoothFactor` | Replaced by PD controller (Kp + Kd). Exponential decay was slow, inaccurate, and had no damping. |
| `sensitivity` | Redundant — `Output = (P+D) × sensitivity` means sensitivity and Kp fight each other. Adjust Kp directly instead. |
| `smoothFactor` (exponential decay) | Replaced by PD controller (Kp + Kd). |
| `predictionFrames`, `emaAlpha`, `predictThreshold` | Velocity feedforward was unstable under visual latency. Replaced by dynamic Kp (distance-based boost). |
| `EMA filter` | PD sub-pixel accumulator made it unnecessary. |

---

## 7. DLL Integration (ddll64.dll)

### Loading

```cpp
MouseController mouse;
mouse.Load("ddll64.dll");
// DLL is copied to exe directory by CMake post-build step
// from host/mousedll/ddll64.dll
```

### API Used

| Function | Signature | Call Frequency |
|----------|-----------|----------------|
| `OpenDevice` | `int OpenDevice()` | Once at init |
| `MoveR` | `void MoveR(int dx, int dy)` | Up to 170 Hz |
| `FreeLibrary` | (dtor) | Once at shutdown |

### Requirements

- **Administrator privileges**: `OpenDevice` returns 0 without elevation.
- **64-bit**: `ddll64.dll` is 64-bit; must match 64-bit Host executable.
- **Co-located**: DLL must be next to the exe or in PATH.

---

## 8. Threading & Safety

- **Single-threaded**: All aim logic runs on the main loop thread at 170Hz.
  No mutex inside `AimAtTarget` — config is read once per tick via `GetConfig()`
  (which is a copy from `HttpTuner`'s mutex-protected state).
- **PD state (`m_prevErrorX/Y`, `m_lastTime`)**: Not thread-safe. Only accessed
  from the main loop thread.
- **Web config updates**: `HttpTuner` runs a background thread. Config is read
  via `GetConfig()` (mutex-protected copy) once per tick. The copy is then
  passed by value to `AimAtTarget`.

---

## 9. Performance

| Metric | Value |
|--------|-------|
| Call frequency | 170 Hz (every tick, even if no detection) |
| Heap allocations | **0** — all math on stack, no vectors |
| Float precision | `float` (32-bit) — sufficient for pixel accuracy |
| `sqrt` calls | 1 per tick (distance check) |
| Branching | ~6 branches (guards, deadzone, range, confidence, quantization) |
| Indirection | 1 function pointer call (`m_moveR`) to DLL |
| Latency added | < 1 µs (pure math + one DLL call) |

---

## 10. File Index

| File | Role |
|------|------|
| `host/include/MouseController.h` | `AimConfig` struct, `MouseController` class |
| `host/src/MouseController.cpp` | PD controller implementation, DLL loading |
| `host/src/main.cpp` (~L248–310) | Target selection, aim point, error vector, calling `AimAtTarget` |
| `host/include/HttpTuner.h` | `TuningState` (holds `AimConfig` for web panel) |
| `host/src/HttpTuner.cpp` | Web UI: Kp/Kd sliders, `/api/config` parsing, `/api/state` serialization |
| `host/mousedll/ddll64.dll` | Mouse input DLL (committed to repo) |
