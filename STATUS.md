# Synapse-X — Project Status

> **Date**: 2026-06-17  
> **Phase**: Host & Client production-ready, optimization complete on both sides

---

## 1. What Works

### Host (主机 — 192.168.100.1)

| Module | Status | Notes |
|--------|--------|-------|
| DXGI capture | ✅ | Center-ROI crop, GPU CopySubresourceRegion, auto-rebuild on ACCESS_LOST |
| LZ4 compression | ✅ | `LZ4_compress_fast(accel=5)`, ~0.2ms, pre-allocated buffer |
| UDP send | ✅ | Non-blocking, 4MB SO_SNDBUF, 20B PacketHeader, MTU-safe ≤1420B/datagram |
| UDP reply receive | ✅ | Port 8889, non-blocking drain, model→screen coord mapping |
| Mouse control (ddll64.dll) | ✅ | PD controller with sub-pixel accumulator + 2-frame delay compensation |
| Web tuning panel | ✅ | Port 9999, independent `web/index.html`, real-time Kp/Kd/EMA sliders |
| Web oscilloscope | ⚠️ | Code in place, last JS scope bug fixed pending test |
| Fixed 170Hz cadence | ✅ | `timeBeginPeriod(1)` + `sleep_until`, TIME_CRITICAL priority, core 2 pinned |
| Configurable ROI | ✅ | CLI: `SynapseX_Host.exe [ip] [port] [w] [h]`, defaults 640×640 |
| Head/body aim | ✅ | `aimPoint` toggle, `headOffset` 0.05–0.25 |

### Client (副机 — 192.168.100.2)

| Module | Status | Notes |
|--------|--------|-------|
| UDP receive + reassembly | ✅ | Non-blocking drain, out-of-order chunk assembly, 20B header |
| LZ4 decompression | ✅ | Dynamic ROI from width/height in PacketHeader |
| TensorRT inference | ✅ | YOLO FP16, 416×416, bf416.engine, **~1.5ms** (was ~3ms) |
| GPU preprocess (NVRTC) | ✅ | BGRA→FP32 CHW on GPU, **~15-30μs**, zero CPU cost |
| Reply send | ✅ | UDP 8889, ReplyHeader + DetectionRaw[] |
| Async producer-consumer | ✅ | LIFO size-1 queue, dual-thread, core affinity (P=core0, C=core1) |
| CUDA stream | ✅ | Dedicated non-blocking stream, 50-frame warmup |
| Inference stability | ✅ | GPU P-State locked, **1.5–1.8ms steady** (was 3–27ms spikes) |

---

## 2. What Changed (v1 → v2)

### Host (v1 → current)

- **PD controller** replaced exponential decay (`smoothFactor 0.15`)
- **Sub-pixel accumulator** replaced forced ±1 quantization → smooth tracking
- **2-frame delay compensation** — subtracts in-flight MoveR from visual error
- **Spatial target lock** (anti-ping-pong) — Phase A maintain / Phase B acquire, 80px radius, 5-frame tolerance
- **Auto-stretch compensation** — dropdown selects game resolution, auto-computes scaleX/Y
- **modelId in protocol** — PacketHeader 20B→24B, modelId embedded per-frame, Client can switch models at runtime
- **Removed `sensitivity`** — redundant with Kp
- **Removed EMA** — PD sub-pixel accumulator made it unnecessary
- **Headers split out** — `web/index.html` served from disk, editable without recompile
- **Default Kp=0.26, Kd=0.05** — tested optimal
- **LZ4 accel 1→5** — ~50% CPU reduction under game load
- **UDP 4MB buffer + non-blocking** — eliminates `sendto` stalls
- **Thread pinned to core 2, TIME_CRITICAL** — eliminates cache thrashing

### Client (v1 → v3)

- **v2: Async dual-thread** — Producer (core 0) + Consumer (core 1), LIFO size-1
- **v3: GPU preprocess (NVRTC)** — BGRA→FP32 on GPU, ~15-30μs, zero CPU
- **GPU P-State locked** — inference stable at 1.5–1.8ms (eliminated 3–27ms spikes)
- **50-frame warmup** + dedicated CUDA stream
- **Dynamic ROI** from PacketHeader

---

## 3. Active Issues

### Host

| # | Issue | Severity | Plan |
|---|-------|----------|------|
| A4 | **Lock not tight** — crosshair drifts during target movement | Medium | Velocity prediction / leading (Kalman filter) |
| B1 | **Linear PD feels robotic** | Low | Noise injection, variable Kp per distance bracket |
| B2 | **No recoil compensation** | Low | Per-game recoil table |

### Client

| # | Issue | Severity | Plan |
|---|-------|----------|------|
| I5 | **Detection bbox no frame smoothing** | Low | IoU matching + EMA on bbox corners |

### Blocked / Needs Investigation

| # | Issue |
|---|-------|
| FW | **Client cannot access Host web panel** (192.168.100.1:9999) — needs Windows firewall rule on Host |
| BF | **Exclusive fullscreen games** — DXGI capturer gets black frames, must use Borderless Windowed |

---

## 4. Tuning Cheat Sheet

```
调参顺序:
  1. Kd=0, 调 Kp → 取"刚好不晃"的最大值（默认 0.26）
  2. 加 Kd 0.01 步进 → 取"刚好不晃"的最小值（默认 0.05）
  3. aimRange 缩到交战距离
  4. minConfidence: 0.25 起，看假阳性/漏检调整
  5. Game Resolution 下拉框选对分辨率 → 自动拉伸补偿

一句话: Kp=速度, Kd=刹车, aimRange=范围
```

| Scenario | Kp | Kd |
|----------|----|----|
| SMG close | 0.40 | 0.08 |
| Rifle mid | 0.26 | 0.05 |
| Sniper far | 0.15 | 0.02 |

---

## 5. File Layout (current)

```
Synapse-X/
├── README.md
├── STATUS.md                        ← this file
├── .gitignore
├── CMakeLists.txt
│
├── shared/include/
│   ├── PacketHeader.h               (24B, width/height/modelId)
│   └── ReplyPacket.h                (ReplyHeader + DetectionRaw)
│
├── host/
│   ├── include/
│   │   ├── DxgiCapturer.h           GPU ROI capture
│   │   ├── Lz4Compressor.h          LZ4 block compression
│   │   ├── UdpSender.h              UDP fragmentation + send
│   │   ├── UdpReplyReceiver.h       UDP reply listener + coord mapping
│   │   ├── MouseController.h        PD controller + sub-pixel + delay-comp
│   │   └── HttpTuner.h              Web tuning panel server
│   ├── src/                         (7 .cpp + main.cpp)
│   ├── web/index.html               Frontend (served from disk, no recompile)
│   ├── test/test_bmp.cpp
│   ├── mousedll/ddll64.dll          Mouse input (committed to repo)
│   ├── CMakeLists.txt
│   ├── CMakePresets.json
│   ├── HOST_SPEC.md                 Full host specification
│   └── MOUSE_CONTROL_SPEC.md        Mouse control deep-dive
│
├── client/
│   ├── include/                     (ReassemblyBuffer, UdpReceiver, TrtInference, CudaPreprocess, UdpReplySender)
│   ├── src/                         (5 .cpp + main.cpp)
│   ├── model/                       (bf416.onnx, bf416.engine)
│   ├── CMakeLists.txt
│   └── CLIENT_SPEC.md
│
└── thirdparty/
    ├── lz4-1.10.0/                  (compiled directly)
    └── cpp-httplib-0.47.0/          (single header)
```

---

## 6. Build & Run

```powershell
# Host
cd host
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
.\build_x64\RelWithDebInfo\SynapseX_Host.exe 192.168.100.2 8888 416 416

# Client
cd client
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
.\build_x64\RelWithDebInfo\SynapseX_Client.exe 8888 ..\..\model\bf416.engine 192.168.100.1

# Web panel
http://localhost:9999  (或 http://192.168.100.1:9999)

# Firewall (if Client can't access Host web panel)
netsh advfirewall firewall add rule name="SynapseX Web Tuner" dir=in action=allow protocol=TCP localport=9999
```

---

## 7. Next Steps

1. **Velocity prediction** (A4) — lead moving targets (Kalman filter)
2. **Recoil system** (B2) — per-game recoil tables
3. **Multi-model support** — switch between 416/640 engines at runtime
4. **Per-game config profiles** — save/load AimConfig as JSON
5. **Hotkey toggle** — bindable aim on/off
6. **Client bbox frame smoothing** (I5) — IoU matching + EMA
