# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Synapse-X is an ultra-low-latency dual-machine AI visual inference pipeline with real-time aim assist. A **Host** (gaming PC) captures the screen at 170Hz, compresses frames with LZ4, and sends them over a dedicated Ethernet cable to a **Client** (inference PC) running TensorRT YOLO. The Client returns detection coordinates, and the Host drives mouse movement via `ddll64.dll`.

All comments and documentation are in Chinese. Code identifiers are in English.

## Build Commands

Host and Client are built **independently** on separate physical machines. There is no top-level build.

### Host (gaming machine)
```powershell
cd host
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
```

### Client (inference machine, requires CUDA + TensorRT)
```powershell
cd client
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
```

### Test program (Host only)
The Host build also produces `SynapseX_Host_TestBmp.exe` — a standalone BMP capture + LZ4 compression test. Built as part of the Host build above.

### Client inference test
```powershell
cd client\test
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
```
Produces `test_infer.exe` which runs TensorRT inference against test images (`test/image/*.jpg`) and compares against expected results (`test/result/*_detections.txt`). Links WIC and COM for image loading.

## Architecture

```
Host (gaming PC)                          Client (inference PC)
┌──────────────────────┐                  ┌─────────────────────────┐
│ DXGI → LZ4 → UDP :8888 ────dedicated───→ Reassembly → LZ4 decode │
│                      │     Ethernet     │                         │
│ MouseCtrl ← UDP :8889 ←───────────────── TensorRT YOLO inference  │
│                      │                  │                         │
│ HttpTuner :9999      │                  │                         │
└──────────────────────┘                  └─────────────────────────┘
```

### Shared Protocol (`shared/include/`)

Header-only library used by both sides via relative path `../shared/include/`.

- **`PacketHeader.h`** — Host→Client frame metadata (24 bytes): magic `0x5358`, frameId, chunking info, ROI dimensions, modelId. Max payload 1400 bytes per UDP datagram.
- **`ReplyPacket.h`** — Client→Host detection results: `ReplyHeader` (16 bytes, magic `0x5359`) + up to 50 `DetectionRaw` structs (24 bytes each: x1,y1,x2,y2,confidence,classId).
- **`Log.h`** — spdlog-based logging setup with console + rotating file sinks. Defines `SX_LOG_*` macros (`SX_LOG_TRACE` through `SX_LOG_CRITICAL`). Call `SynapseX::Log::Initialize(appName)` at startup.

### Host Modules (`host/`)

| Module | Header | Role |
|--------|--------|------|
| DxgiCapturer | `include/DxgiCapturer.h` | DXGI Desktop Duplication, GPU-side ROI crop, 170Hz fixed-rate capture, thread pinning to P-Core |
| Lz4Compressor | `include/Lz4Compressor.h` | LZ4 block compression with `LZ4_compress_fast(accel=5)` |
| UdpSender | `include/UdpSender.h` | Chunks compressed frame into ≤1400B packets with PacketHeader, non-blocking 4MB buffer |
| UdpReplyReceiver | `include/UdpReplyReceiver.h` | Listens on UDP :8889, deserializes ReplyHeader + DetectionRaw[], maps coordinates to screen space |
| MouseController | `include/MouseController.h` | PD controller with dynamic Kp, Kd damping, sub-pixel accumulator, 2-frame delay compensation, loads `ddll64.dll` |
| HttpTuner | `include/HttpTuner.h` | Embedded HTTP server on :9999 serving `web/index.html` for real-time parameter tuning from phone/tablet |

### Client Modules (`client/`)

| Module | Header | Role |
|--------|--------|------|
| UdpReceiver | `include/UdpReceiver.h` | UDP recv on :8888, decompresses with LZ4 |
| ReassemblyBuffer | `include/ReassemblyBuffer.h` | Out-of-order packet reassembly by frameId |
| TrtInference | `include/TrtInference.h` | TensorRT YOLO inference, FP16, 416×416, loads `.engine` file |
| CudaPreprocess | `include/CudaPreprocess.h` | GPU-side image preprocessing via NVRTC |
| UdpReplySender | `include/UdpReplySender.h` | Sends detection results back to Host on :8889 |

### Key Architectural Details

- **Thread model**: Host capture thread is pinned to a P-Core with `TIME_CRITICAL` priority for stable 170Hz cadence. Client uses async producer-consumer: Producer (core 0) handles network I/O, Consumer (core 1) handles GPU inference on a dedicated CUDA stream, with a **LIFO slot (size 1)** between them — stale frames are dropped to prevent queue-head blocking.
- **Model switching**: `g_targetModelId` (atomic uint8_t in `PacketHeader.h`) is written by UdpReceiver when a new frame arrives and read by TrtInference at the start of each `Infer()` call. This allows runtime model switching (e.g., PUBG ↔ AimLab) without restart. The `client/model/` directory contains 8 `.engine` and 8 `.onnx` files for 6 games: Apex, Delta Force, Battlefield 6, Overwatch 2, Aimlabs, PUBG.
- **Hotkeys**: PageUp toggles aim assist on, PageDown toggles it off (handled in the Host main loop).
- **Anti-degradation**: Non-blocking UDP sockets, dynamic LZ4 acceleration, and core pinning prevent pipeline collapse under game load.
- **DXGI recovery**: On `DXGI_ERROR_ACCESS_LOST`, the entire capture chain is rebuilt automatically.

## Dependencies

- **LZ4 1.10.0** (`thirdparty/lz4-1.10.0/`) — compiled directly from source as a static library
- **cpp-httplib 0.47.0** (`thirdparty/cpp-httplib-0.47.0/`) — single-header HTTP server, included directly
- **spdlog 1.17.0** (`thirdparty/spdlog-1.17.0/`) — header-only logging, included via `SynapseX_Shared` interface target
- **CUDA 13.1 + TensorRT 10.16.1.11** — Client only, hardcoded path in `client/CMakeLists.txt`: `C:/Program Files/NVIDIA/TensorRT-10.16.1.11.Windows.amd64.cuda-13.2/TensorRT-10.16.1.11`
- **Windows SDK 10.0.26100** — D3D11, DXGI, Winsock2, winmm
- **Visual Studio 2026** (v18) — MSVC toolchain, C++17

## Network Topology

- Dedicated Ethernet direct connection (no switch/router)
- Host IP: `192.168.100.1`, Client IP: `192.168.100.2`
- Host → Client: UDP port 8888 (frame data)
- Client → Host: UDP port 8889 (detection results)
- Host HTTP: TCP port 9999 (tuner web panel)

## Runtime

```powershell
# Client first
.\SynapseX_Client.exe 8888 ..\..\model\bf416.engine 192.168.100.1

# Host (requires Administrator for mouse control)
.\SynapseX_Host.exe 192.168.100.2 8888 416 416

# Web tuner: http://192.168.100.1:9999
```

The Host copies `mousedll/ddll64.dll` and `web/` to the build output via CMake post-build commands.

## Important Constraints

- The game must run in **borderless window** mode (fullscreen exclusive causes DXGI to return black frames).
- Mouse control requires **Administrator privileges**.
- The `.engine` file path in `client/model/` is a symlink/gitignored — actual engine files are stored in `client/model/engine/`.
- `ddll64.dll` is committed to the repo (`!host/mousedll/ddll64.dll` in `.gitignore` exception) — it's a required runtime dependency, not built from source.
- Spec documents are at `host/HOST_SPEC.md` and `client/CLIENT_SPEC.md`. They are the authoritative technical references for each side of the pipeline.
- For Python scripts (model export, ONNX conversion, etc.), activate the conda environment first: `conda activate yolo26`.
