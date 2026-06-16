# Synapse-X — Project Status

> **Date**: 2026-06-16  
> **Phase**: Host & Client functional, tuning in progress

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
| TensorRT inference | ✅ | YOLO FP16, 416×416, bf416.engine, ~3ms typical |
| Reply send | ✅ | UDP 8889, ReplyHeader + DetectionRaw[] |
| Async producer-consumer | ✅ | LIFO size-1 queue, dual-thread, core affinity |
| CUDA stream | ✅ | Dedicated non-blocking stream, 50-frame warmup |
| Per-second stats | ✅ | FPS, drop rate, LIFO drops, throughput |

---

## 2. What Changed (v1 → v2)

### Host

- **PD controller** replaced exponential decay (`smoothFactor 0.15`)
- **Sub-pixel accumulator** replaced forced ±1 quantization → smooth tracking
- **2-frame delay compensation** — subtracts in-flight MoveR from visual error
- **EMA low-pass filter** (`emaAlpha 0.20`) between YOLO output and PD — suppresses bbox flutter
- **Removed `sensitivity` parameter** — was redundant with Kp, caused tuning confusion
- **LZ4 accel 1→5** — trades ~5% compression for ~50% CPU reduction under game load
- **UDP 4MB buffer + non-blocking** — eliminates `sendto` stalls
- **Thread pinned to core 2, TIME_CRITICAL** — eliminates cache thrashing

### Client

- **Async dual-thread architecture** — Producer (core 0, UDP) + Consumer (core 1, GPU)
- **LIFO size-1 queue** — zero backlog, always newest frame
- **50-frame black image warmup** — forces GPU P-State + JIT compilation
- **Dynamic ROI** — reads width/height from PacketHeader

---

## 3. Active Issues

### Host

| # | Issue | Severity | Plan |
|---|-------|----------|------|
| A3 | **Target switching** — jumps between enemies when confidence flickers | Medium | Lock-on: require N frames before switching, hysteresis margin |
| A4 | **Lock not tight** — crosshair drifts during target movement | Medium | Velocity prediction / leading (Kalman or EMA of position deltas) |
| B1 | **Linear decay feels robotic** | Low | Noise injection, variable smoothFactor per distance bracket |
| B2 | **No recoil compensation** | Low | Per-game recoil table |
| WS | **Web oscilloscope** — pending test after last JS fix | Low | Verify scope renders after JSON fix |

### Client

| # | Issue | Severity | Plan |
|---|-------|----------|------|
| C1 | **Inference spikes 3→17–27ms** | Medium | Lock GPU clocks via `nvidia-smi -lgc`, check driver DPC latency |
| C2 | **Inference variance causes Host aim jitter** | Medium | Consequence of C1; fix C1 first |
| I1 | **CPU preprocess bottleneck** (BGRA→FP32 loop) | Low | Move to CUDA kernel |
| I5 | **Detection bbox no frame smoothing** | Low | IoU matching + EMA on bbox corners |

### Blocked / Needs Investigation

| # | Issue |
|---|-------|
| FW | **Client cannot access Host web panel** (192.168.100.1:9999) — needs Windows firewall rule on Host |
| BF | **Exclusive fullscreen games** — DXGI capturer gets black frames, must use Borderless Windowed |

---

## 4. Tuning Cheat Sheet

```
调参顺序（严格遵守）:
  0. 关 Kd=0, emaAlpha=1.0, aimRange=1000 → 看原始信号
  1. Kp: 取"刚好不晃"的最大值（通常 0.2–0.5）
  2. emaAlpha: 看 Web 面板 target.dist 值 → 0.2 起，静止时准星不抖即可
  3. Kd: 0.02 起 +0.01 步进 → 取"刚好不晃"的最小值
  4. aimRange: 缩到你的交战距离
  5. minConfidence: 0.25 起，看假阳性/漏检调整

一句话: Kp=速度, Kd=刹车, emaAlpha=平滑, aimRange=范围
```

| Scenario | Kp | Kd | emaAlpha |
|----------|----|----|----|
| SMG close | 0.5 | 0.08 | 0.15 |
| Rifle mid | 0.35 | 0.05 | 0.20 |
| Sniper far | 0.2 | 0.02 | 0.10 |
| Heavy flutter | 0.4 | 0.05 | 0.08 |

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
│   ├── PacketHeader.h               (20B, width/height)
│   └── ReplyPacket.h                (ReplyHeader + DetectionRaw)
│
├── host/
│   ├── include/
│   │   ├── DxgiCapturer.h
│   │   ├── Lz4Compressor.h
│   │   ├── UdpSender.h
│   │   ├── UdpReplyReceiver.h
│   │   ├── MouseController.h
│   │   └── HttpTuner.h
│   ├── src/                         (6 .cpp + main.cpp)
│   ├── web/index.html               (独立前端 — 直接编辑, 无需重编译)
│   ├── test/test_bmp.cpp
│   ├── mousedll/ddll64.dll
│   ├── CMakeLists.txt
│   ├── CMakePresets.json
│   ├── HOST_SPEC.md
│   └── MOUSE_CONTROL_SPEC.md
│
├── client/
│   ├── include/                     (ReassemblyBuffer, UdpReceiver, TrtInference, UdpReplySender)
│   ├── src/                         (4 .cpp + main.cpp)
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

## 7. Next Steps (priority order)

1. **Test web oscilloscope** — verify scope renders after JSON fix
2. **Fix firewall** — enable Client access to Host :9999
3. **Target lock-on** (A3) — single biggest aim quality improvement
4. **Velocity prediction** (A4) — lead moving targets
5. **Client GPU lock** (C1) — eliminate inference spikes
6. **Client preprocess GPU** (I1) — BGRA→FP32 on CUDA kernel
7. **Recoil system** (B2) — per-game recoil tables
8. **Multi-model support** — switch between 416/640 engines at runtime
