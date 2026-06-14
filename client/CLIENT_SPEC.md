# Synapse-X Client Specification

> **Target audience**: Host (capturing machine) developer.
> This document describes every detail of the Client-side pipeline:
> UDP receive → out-of-order reassembly → LZ4 decompress → TensorRT inference.
> Use this alongside `host/HOST_SPEC.md` for joint debugging.

---

## 1. System Overview

```
┌── HOST (capturing machine) ─────────────────────────────────────────┐
│                                                                      │
│  DXGI capture (ROI) → LZ4 compress → fragment → UDP send            │
│                                                                      │
└───────┬──────────────────────────────────────────────────────────────┘
        │  Ethernet (physically isolated, direct or switch)
        ▼
┌── CLIENT (this machine) ────────────────────────────────────────────┐
│                                                                      │
│  ┌─ Stage 1: UDP recv (non-blocking drain) ─────────────────────┐   │
│  │  · recvfrom() loop, validate magic=0x5358                      │   │
│  └───────────────────────┬───────────────────────────────────────┘   │
│                          ▼                                           │
│  ┌─ Stage 2: ReassemblyBuffer ──────────────────────────────────┐   │
│  │  · Out-of-order chunk insertion (offset = idx × 1400)          │   │
│  │  · Duplicate detection via bitmask                              │   │
│  │  · New frameId → discard old partial frame (iron law)          │   │
│  └───────────────────────┬───────────────────────────────────────┘   │
│                          ▼                                           │
│  ┌─ Stage 3: LZ4 Decompress ────────────────────────────────────┐   │
│  │  · LZ4_decompress_safe → verify output == width × height × 4   │   │
│  │  · Dynamic ROI: 416², 640², etc. from PacketHeader             │   │
│  └───────────────────────┬───────────────────────────────────────┘   │
│                          ▼                                           │
│  ┌─ Stage 4: TensorRT Inference ─────────────────────────────────┐   │
│  │  · BGRA → FP32 CHW RGB [0,1] (CPU preprocess)                  │   │
│  │  · CUDA memcpy H→D → enqueueV3 → memcpy D→H                    │   │
│  │  · Postprocess: threshold + clamp to model dims                 │   │
│  │  · Output: Detection[] in model pixel coordinates               │   │
│  └───────────────────────┬───────────────────────────────────────┘   │
│                          ▼                                           │
│  ┌─ Future: UDP reply to Host ───────────────────────────────────┐   │
│  │  · Detection coords + frameId → send back to Host               │   │
│  └────────────────────────────────────────────────────────────────┘   │
│                                                                      │
└──────────────────────────────────────────────────────────────────────┘
```

### Key Numbers

| Parameter | Value |
|-----------|-------|
| **Network** | |
| Listen port | 8888 (configurable) |
| Socket mode | Non-blocking (`FIONBIO`) |
| Receive buffer | 256 KB |
| Protocol header | 20 bytes (dynamic ROI: width/height at offset 16/18) |
| UDP datagram max | 1420 bytes (20 header + 1400 payload) |
| **Decompression** | |
| ROI support | Any `width × height` from PacketHeader (max 4096²) |
| Raw frame size | `width × height × 4` (dynamic) |
| LZ4 function | `LZ4_decompress_safe` (raw block, NOT frame format) |
| Max compressed | 67,372,052 bytes (`LZ4_compressBound(4096²×4)`) |
| **Inference** | |
| Model input | `1 × 3 × 416 × 416` FP32 CHW RGB |
| Model output | `1 × 300 × 6` FP32 (`[x1,y1,x2,y2,conf,cls]`) |
| TensorRT version | 10.16.1.11 |
| Build precision | FP16 |
| GPU Compute | ~1.57 ms (measured on trtexec benchmark) |
| CUDA version | 13.1.115 |
| Engine file | `model/bf416.engine` (7.4 MB) |

---

## 2. Module Architecture

```
client/
├── CLIENT_SPEC.md               ← this file
├── CMakeLists.txt               ← standalone CMake project
├── CMakePresets.json            ← VS 2026 x64 preset
├── model/
│   ├── bf416.onnx               ← ONNX model (9.3 MB, portable)
│   └── bf416.engine             ← TRT engine (7.4 MB, GPU-bound)
├── include/
│   ├── ReassemblyBuffer.h       ← out-of-order reassembly engine
│   ├── UdpReceiver.h            ← UDP recv + LZ4 decompress
│   └── TrtInference.h           ← TensorRT inference wrapper
└── src/
    ├── UdpReceiver.cpp          ← receive / reassemble / decompress
    ├── TrtInference.cpp         ← engine load / preprocess / infer / postprocess
    └── main.cpp                 ← full pipeline loop: stats, BMP, detection print
```

### Dependency Graph

```
main.cpp
  ├── UdpReceiver.h
  │     └── ReassemblyBuffer.h
  │           └── PacketHeader.h  (shared/)
  └── TrtInference.h
        └── (CUDA + TensorRT SDK)
```

### Build

```powershell
cd client
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
# Binary: build_x64/RelWithDebInfo/SynapseX_Client.exe
```

### Build Requirements

| Component | Required | Path (auto-detected) |
|-----------|----------|----------------------|
| Visual Studio 2026 | Yes | CMake generator |
| LZ4 | Yes | `../thirdparty/lz4-1.10.0/` (compiled in-tree) |
| CUDA 13.1 | Yes (for inference) | `C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.1` |
| TensorRT 10.16 | Yes (for inference) | `C:/Program Files/NVIDIA/TensorRT-10.16.1.11.../` |
| Winsock2 | Yes | Windows SDK |

If CUDA or TensorRT is missing, the build succeeds but `SynapseX_Client.exe` runs
in **receive-only mode** (UDP + LZ4 decompress, no inference).

### Run

```powershell
cd client\build_x64\RelWithDebInfo

# Receive + inference:
.\SynapseX_Client.exe 8888 ..\..\model\bf416.engine

# Receive-only (no engine or bad path):
.\SynapseX_Client.exe 8888
```

Arguments: `SynapseX_Client.exe [port] [enginePath]`

---

## 3. ReassemblyBuffer — Out-of-Order Reassembly Engine

**File:** `client/include/ReassemblyBuffer.h`

### 3.1 Data Structure

```cpp
struct ReassemblyBuffer {
    uint32_t expectedFrameId;     // frameId currently being collected (0xFFFFFFFF = none)
    uint32_t totalSize;           // compressed size (from header.totalSize)
    uint16_t totalChunks;         // expected number of chunks
    uint16_t chunksReceived;      // unique chunks collected so far
    uint16_t frameWidth;          // ROI width  (from PacketHeader, for decompress verify)
    uint16_t frameHeight;         // ROI height (from PacketHeader, for decompress verify)

    uint32_t GetRawFrameSize() const { return frameWidth * frameHeight * 4; }

    std::vector<bool>    receivedMask;  // bitmask: which chunks arrived
    std::vector<uint8_t> data;          // reassembly buffer (totalSize bytes)
};
```

### 3.2 Core Algorithm

#### Chunk Placement — Offset Formula

```
offset = chunkIndex × MAX_PAYLOAD_SIZE  (= chunkIndex × 1400)
```

All chunks except the last are exactly 1400 bytes. The offset formula maps
`chunkIndex` directly to byte position regardless of arrival order.

Example: compressed frame of 3,000 bytes → 3 chunks:

| chunkIndex | offset | payloadSize |
|------------|--------|-------------|
| 0 | 0 | 1400 |
| 1 | 1400 | 1400 |
| 2 | 2800 | 200 |

#### Frame Transition — The Iron Law

```
if (frameId > expectedFrameId):
    → discard old partial frame IMMEDIATELY (count as dropped)
    → start new frame with width/height from PacketHeader

if (frameId < expectedFrameId):
    → drop (stale packet from old frame)

if (frameId == expectedFrameId):
    → insert chunk at correct offset
```

Zero head-of-line blocking. The moment a newer `frameId` arrives, the old
frame is irrevocably abandoned — never wait for stragglers.

#### Duplicate Detection

```cpp
if (receivedMask[chunkIndex] == true):
    → drop (duplicate packet, already received)
```

#### Completion Check

```cpp
if (chunksReceived == totalChunks && totalChunks > 0):
    → frame is complete, ready for decompression
```

### 3.3 Memory Strategy

All buffers are pre-allocated to worst-case (4096² ROI) in the constructor:

| Buffer | Pre-allocation | Notes |
|--------|---------------|-------|
| `data` | 67,372,052 bytes | `LZ4_compressBound(4096²×4)`. In practice frames are 30–400 KB compressed. |
| `receivedMask` | 48,123 bits (≈6 KB) | `ceil(67372052 / 1400)`. Reset per-frame via `assign()`. |

`StartFrame()` calls `data.resize(totalSz)` only if needed — with the
worst-case pre-allocation this never fires in practice. **Zero heap
allocation in the receive hot path.**

### 3.4 Frame ID Wrap-Around

```cpp
inline bool IsNewerFrameId(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) > 0;
}
```

Signed-difference comparison. Correctly handles `uint32_t` wrap from
`0xFFFFFFFF` → `0x00000000`. At 170 Hz this wraps after ~290 days.

---

## 4. UdpReceiver — UDP Receive & Decompress Pipeline

**Files:** `client/include/UdpReceiver.h`, `client/src/UdpReceiver.cpp`

### 4.1 Lifecycle

```
Initialize(port)          bind socket, set non-blocking, pre-allocate buffers
       │
       ▼
┌── TryReceive() loop ──────────────────────────────────────────┐
│                                                                │
│  1. recvfrom() in non-blocking drain loop (WSAEWOULDBLOCK = done) │
│       │                                                        │
│       ▼                                                        │
│  2. ProcessDatagram() per packet:                              │
│     · Validate magic == 0x5358                                 │
│     · Validate header truncated check                          │
│     · Extract width/height from 20-byte PacketHeader           │
│     · Check frameId transition (newer/stale/same)              │
│     · InsertChunk() into ReassemblyBuffer                      │
│       │                                                        │
│       ▼                                                        │
│  3. If frame complete:                                         │
│     · LZ4_decompress_safe → verify == width×height×4           │
│     · Record m_lastFrameWidth / m_lastFrameHeight              │
│     · Gap detection (frameId jumps)                            │
│     · Reset buffer for next frame                              │
│                                                                │
└────────────────────────────────────────────────────────────────┘
       │
       ▼
Cleanup()                  close socket, release WinSock
```

### 4.2 Socket Configuration

| Setting | Value | Rationale |
|---------|-------|-----------|
| Mode | `FIONBIO` (non-blocking) | Drain all queued datagrams without blocking |
| Receive buffer | 256 KB (`SO_RCVBUF`) | Absorb bursts from Host's tight send loop |
| Bind address | `INADDR_ANY` (0.0.0.0) | Accept from any network interface |
| Protocol | `IPPROTO_UDP` | Raw UDP datagrams |

### 4.3 Per-Packet Processing (`ProcessDatagram`)

```
Input: raw UDP datagram (data, len)

1. If len < 20:
     → drop (too small for 20-byte PacketHeader)

2. Cast first 20 bytes → PacketHeader*
   If header.magic != 0x5358:
     → drop (not our protocol)

3. Extract payload:
     payload     = &data[20]
     payloadSize = header.payloadSize

4. Safety check:
     If (20 + payloadSize) > len:
       → drop with warning (truncated datagram)

5. Frame transition:
     if !HasActiveFrame():
       → StartFrame(frameId, totalSize, totalChunks, width, height)
     elif IsNewerFrameId(frameId, expectedFrameId):
       → if old frame was incomplete: m_totalDropped++
       → StartFrame(frameId, totalSize, totalChunks, width, height)
     elif frameId != expectedFrameId:
       → drop (stale)

6. InsertChunk(chunkIndex, payload, payloadSize)
   If duplicate → drop

7. If IsComplete():
     → gap detection (see §5.1)
     → LZ4_decompress_safe → verify == width×height×4
     → if OK: m_totalFramesReceived++, cache dimensions, return true
     → if FAIL: m_totalDropped++, reset
```

### 4.4 Decompression

```cpp
outFrame.resize(frameWidth * frameHeight * 4);  // dynamic, reused across frames

int result = LZ4_decompress_safe(
    compressedData,          // reassembled from chunks
    outFrame.data(),         // width×height×4 capacity
    compressedSize,          // from header.totalSize
    frameWidth * frameHeight * 4
);

if (result != frameWidth * frameHeight * 4) {
    // corruption or size mismatch → discard frame
}
```

`outFrame` (`m_decompressBuf`) is resized lazily — only reallocates when ROI
dimensions increase. Same-size consecutive frames cause zero allocation.

### 4.5 API

```cpp
class UdpReceiver {
public:
    bool Initialize(uint16_t port = 8888);
    void Cleanup();

    // Hot path — call at high frequency (≥ every 1 ms).
    // Returns true if a complete frame was decoded.
    bool TryReceive(std::vector<uint8_t>& outFrame, uint32_t& outFrameId);

    // Valid after TryReceive returns true.
    uint16_t GetLastFrameWidth()  const;
    uint16_t GetLastFrameHeight() const;

    // Cumulative stats.
    uint64_t GetTotalFrames()  const;
    uint64_t GetTotalDropped() const;
    uint64_t GetTotalPackets() const;
    uint64_t GetTotalBytes()   const;
};
```

---

## 5. TrtInference — TensorRT Inference Module

**Files:** `client/include/TrtInference.h`, `client/src/TrtInference.cpp`

### 5.1 Lifecycle

```
Initialize(enginePath, 416, 416, 300)
  │
  ├─ Load engine file from disk
  ├─ deserializeCudaEngine()
  ├─ createExecutionContext()
  ├─ Allocate GPU input/output buffers (cudaMalloc)
  └─ setInputTensorAddress / setOutputTensorAddress
       │
       ▼
Infer(bgra, confThr)  ← per-frame hot path
  │
  ├─ 1. Preprocess: BGRA uint8 → FP32 CHW RGB [0,1] (CPU)
  ├─ 2. cudaMemcpy H→D
  ├─ 3. enqueueV3() — GPU inference
  ├─ 4. cudaMemcpy D→H
  └─ 5. Postprocess: threshold filter + clamp to [0, modelW/H)
       │
       ▼
Cleanup()
  ├─ cudaFree input / output
  ├─ delete context
  ├─ delete engine
  └─ delete runtime
```

### 5.2 Preprocessing Detail (BGRA → FP32 CHW RGB)

The Host sends BGRA pixels (DXGI_FORMAT_B8G8R8A8_UNORM). The model expects
FP32 CHW RGB normalized to [0, 1]. Conversion is done on CPU (416×416×3
≈ 2 MB — well within L3 cache):

```
For each pixel (x, y) in modelW × modelH:
    src = (y * modelW + x) * 4     // BGRA uint8 offset
    R = bgra[src + 2] / 255.0f     // B→R channel swap
    G = bgra[src + 1] / 255.0f     // G→G (no swap)
    B = bgra[src + 0] / 255.0f     // R→B channel swap
    // Alpha (src+3) is ignored

Output layout: float input[3 * modelW * modelH]
    Channel 0 (R): input[0 * planeSize + y * modelW + x]
    Channel 1 (G): input[1 * planeSize + y * modelW + x]
    Channel 2 (B): input[2 * planeSize + y * modelW + x]
```

### 5.3 Postprocessing Detail

```cpp
// Coordinates stay in model pixel space — NO scaling applied.
// Host knows the ROI size and can map back to screen coords itself.

for each detection row [x1, y1, x2, y2, conf, cls]:
    if (conf < confThr): skip
    clamp x1/x2 to [0, modelW - 1]
    clamp y1/y2 to [0, modelH - 1]
    emit Detection { x1, y1, x2, y2, conf, classId }
```

### 5.4 Model Output Format

| Field | Type | Range | Meaning |
|-------|------|-------|---------|
| `x1, y1` | float32 | [0, 416) | Top-left corner, model pixel coords |
| `x2, y2` | float32 | [0, 416) | Bottom-right corner, model pixel coords |
| `conf` | float32 | [0, 1] | Detection confidence |
| `classId` | int (float) | 0 or 1 | 0 = enemy, 1 = teammate |

### 5.5 API

```cpp
class TrtInference {
public:
    bool Initialize(const std::string& enginePath,
                    int modelWidth = 416,
                    int modelHeight = 416,
                    int numDetections = 300);

    // Returns detections in model pixel coordinates (no scaling).
    std::vector<Detection> Infer(const uint8_t* bgra,
                                 float confThr = 0.25f);

    void Cleanup();
    bool IsInitialized() const;
    int GetModelWidth()  const;
    int GetModelHeight() const;
};

struct Detection {
    float x1, y1;       // model pixel coords, no scaling
    float x2, y2;
    float confidence;
    int   classId;      // 0 = enemy, 1 = teammate
};
```

---

## 6. Drop Detection & Statistics

### 6.1 Two Categories of Drops

**Type A — Abandoned partial frame:**
Newer `frameId` arrives before current frame is complete → old frame discarded.

```
Frame N: chunks 0, 1, 3 received (chunk 2 missing)
  → Frame N+1 arrives
  → Frame N abandoned → m_totalDropped++
```

**Type B — Frame ID gap (silent skip):**
Complete frame decoded. Check `frameId` jump since last success:

```cpp
int32_t gap = (frameId - m_lastDecodedFrameId) - 1;
if (gap > 0) {
    m_totalDropped += gap;  // frames we never even started
}
```

### 6.2 Stats Output Format

```
---- per-second stats --------------------------------
  ROI: 416x416  |  FPS:   170.3  |  frames:   170  |  dropped:     2  |  drop rate:   1.2%
  inference:   168/s  |  throughput:   12.34 MB/s  |  total frames: 15230
```

| Field | Meaning |
|-------|---------|
| `ROI` | Current frame dimensions from PacketHeader |
| `FPS` | Successfully decoded frames per second |
| `frames` | Frames decoded in this 1-second window |
| `dropped` | Frames lost in this window (Type A + Type B) |
| `drop rate` | `dropped / (frames + dropped) × 100%` |
| `inference` | TRT inferences completed per second |
| `throughput` | Network throughput in MB/s |
| `total frames` | Cumulative decoded frames since start |

### 6.3 Detection Output (periodic)

Every 30 frames, the Client prints the top 3 detections:

```
[INFER] Frame #150: 5 detections
  [enemy] conf=0.87 box=[120,85,200,180]
  [teammate] conf=0.72 box=[300,150,380,250]
  [enemy] conf=0.55 box=[50,200,90,300]
```

---

## 7. Verification — BMP Output

When the **10th complete frame** is successfully decoded, `main.cpp` writes:

```
client_test.bmp     (W×H from PacketHeader, 32-bit BGRA, top-down DIB)
```

### BMP Format

| Field | Value | Notes |
|-------|-------|-------|
| `bfType` | `0x4D42` ('BM') | |
| `biWidth` | dynamic (416, 640, ...) | From PacketHeader.width |
| `biHeight` | **-height** | Negative = top-down DIB |
| `biBitCount` | 32 | BGRA, 8 bits/channel |
| `biCompression` | `BI_RGB` (0) | Uncompressed |
| Channel order | B, G, R, A | Matches DXGI_FORMAT_B8G8R8A8_UNORM |

The Client also prints the first 4 pixels:
```
[VERIFY] First 4 pixels (B,G,R,A): [ 31, 31, 31,255] [ 31, 31, 31,255] ...
```

---

## 8. Joint Debugging Guide

### 8.1 Loopback Test (Single Machine)

```powershell
# Terminal 1 — Client first
cd client\build_x64\RelWithDebInfo
.\SynapseX_Client.exe 8888 ..\..\model\bf416.engine

# Terminal 2 — Host (416×416 ROI to match model)
cd host\build_x64\RelWithDebInfo
.\SynapseX_Host.exe 127.0.0.1 8888 416 416
```

Expected output:
```
[VERIFY] Frame #10 (Host frameId=9) saved as 'client_test.bmp'
[INFER] Frame #30: 4 detections
  [enemy] conf=0.87 box=[120,85,200,180]
...
---- per-second stats --------------------------------
  ROI: 416x416  |  FPS:    25.0  |  frames:    25  |  dropped:     0  |  drop rate:   0.0%
  inference:    25/s  |  throughput:    5.20 MB/s  |  total frames: 30
```

### 8.2 Physical Isolation Test (Two Machines)

```
Client (192.168.100.2)             Host (192.168.100.1)
.\SynapseX_Client.exe 8888         .\SynapseX_Host.exe 192.168.100.2 8888 416 416
  ..\..\model\bf416.engine
```

### 8.3 Troubleshooting

| Symptom | Likely Cause | Check |
|---------|-------------|-------|
| Client prints nothing | Host not sending, firewall, wrong IP | `netstat -ano \| findstr 8888` on Client |
| `drop rate` > 0% | Network congestion, socket buffer too small | Bump `SO_RCVBUF` to 512 KB |
| `LZ4_decompress_safe ERROR` | Corrupted payload, bit-flip on wire | Compare `compressed=` vs Host log |
| `LZ4 size mismatch` | Chunk reassembly error | Verify `chunkIndex * 1400` on both ends |
| BMP garbled | Wrong dimensions or stride | Check `biWidth`/`biHeight` in BMP header |
| `[WARN] TRT NOT available` | Engine file not found or CUDA/TensorRT missing | Check engine path argument |
| `[WARN] ROI 640x640 != model 416x416` | Host ROI doesn't match model input | Configure Host to send 416×416 |
| `enqueueV3 FAILED` | GPU error, out of memory, or bad engine | Rebuild engine with `trtexec --fp16` |

### 8.4 Diagnostic Log Lines

**Client startup (inference enabled):**
```
[TrtInference] Loaded engine: ../../model/bf416.engine (7.4 MB)
[TrtInference] Ready. Model: 416x416, 300 detections, output: 7200 bytes
[UdpReceiver] Ready — listening on 0.0.0.0:8888, non-blocking, recv buffer 256 KB, dynamic ROI
[INFO] Waiting for data from Host...
```

**Client startup (receive-only fallback):**
```
[WARN] TensorRT inference NOT available. Running in receive-only mode.
[WARN] Check engine path: ../../model/bf416.engine
[UdpReceiver] Ready — listening on 0.0.0.0:8888, ...
```

**ROI mismatch warning (prints once):**
```
[WARN] Frame ROI 640x640 != model 416x416. Skipping inference.
       Configure Host to send 416x416 ROI.
```

---

## 9. File Index

| File | Purpose | Lines |
|------|---------|-------|
| `CMakeLists.txt` | Build config: LZ4, CUDA 13.1, TRT 10.16, ws2_32 | ~210 |
| `CMakePresets.json` | VS 2026 x64 preset | ~35 |
| `model/bf416.onnx` | ONNX model (portable, can regenerate engine) | — |
| `model/bf416.engine` | Serialized TRT engine (GPU-bound) | — |
| `include/ReassemblyBuffer.h` | Reassembly engine (header-only struct) | ~140 |
| `include/UdpReceiver.h` | UDP receiver interface | ~130 |
| `include/TrtInference.h` | TRT inference interface | ~85 |
| `src/UdpReceiver.cpp` | Receive, reassemble, decompress | ~280 |
| `src/TrtInference.cpp` | TRT engine load, pre/infer/post | ~220 |
| `src/main.cpp` | Full pipeline loop, stats, BMP, detection print | ~320 |
| `CLIENT_SPEC.md` | This file | — |
| `deploy_trt.md` | TRT deployment tutorial (reference) | — |

---

## 10. Quick Reference Card

```
┌──────────────────────────────────────────────────────────────────┐
│                 Synapse-X CLIENT — Quick Reference                │
├──────────────────────────────────────────────────────────────────┤
│                                                                   │
│  BUILD:    cd client && cmake --preset windows-x64                │
│            cmake --build build_x64 --config RelWithDebInfo        │
│                                                                   │
│  RUN:      .\build_x64\RelWithDebInfo\SynapseX_Client.exe         │
│              [port=8888] [engine=../../model/bf416.engine]        │
│                                                                   │
│  LISTEN:   UDP 0.0.0.0:8888                                       │
│                                                                   │
│  PROTOCOL: 20-byte PacketHeader per chunk                         │
│            magic=0x5358  chunk offset = chunkIndex × 1400         │
│                                                                   │
│  FORMAT:   W×H BGRA (dynamic), width×height×4 bytes per frame     │
│                                                                   │
│  MODEL:    1×3×416×416 FP32 CHW → 300 detections                  │
│            Coordinates in model pixel space (no scaling)          │
│                                                                   │
│  VERIFY:   10th frame → client_test.bmp                           │
│                                                                   │
│  STATS:    Per-second: ROI, FPS, drop rate, inference, throughput │
│            Per 30 frames: top 3 detections                        │
│                                                                   │
└──────────────────────────────────────────────────────────────────┘
```

---

## 11. Recommendations for Host Developer

### 11.1 ROI Size Must Match Model

The TensorRT engine is compiled for **416×416** input. The Client will skip
inference (with a one-time warning) if the received frame dimensions don't
match. Make sure the Host is configured to send a **416×416 ROI**.

If you want to use a different model size (e.g. 640×640):
1. Export a new ONNX at the new resolution
2. Run `trtexec --onnx=new_model.onnx --saveEngine=new_model.engine --fp16`
3. Update the engine path argument and model dimensions in `main.cpp`

### 11.2 Frame Rate vs. Inference Rate

At 170 Hz Host capture rate, the Client's inference (1.57 ms GPU compute) can
keep up if the network delivers frames reliably. However:

- If Host sends frames faster than GPU can process, `Infer()` calls will queue
  up and latency will accumulate.
- The Client does NOT drop frames before inference — if you want frame skipping
  when inference falls behind, implement a "skip if context still busy" check.
- Consider throttling Host FPS to match the inference throughput ceiling
  (theoretically ~600 FPS for 1.57 ms inference, but memory bandwidth and
  preprocessing add overhead).

### 11.3 Coordinate Mapping

Detections are returned in **model pixel coordinates** `[0, 415] × [0, 415]`.
The Host is responsible for mapping these back to screen coordinates:

```
Given:
  modelW = 416, modelH = 416
  roiX = (screenW - modelW) / 2    // center-crop offset on Host
  roiY = (screenH - modelH) / 2

For each detection:
  screen_x1 = roiX + x1
  screen_y1 = roiY + y1
  screen_x2 = roiX + x2
  screen_y2 = roiY + y2
```

The Host already knows its own ROI configuration — centralizing coordinate
mapping there avoids duplicating the logic on the Client side.

### 11.4 Recommended Client→Host Reply Protocol

When the reply channel is implemented, we suggest a compact binary format
to keep latency minimal:

```
┌────────┬────────┬────────┬────────┬──────────────┐
│ magic  │frameId │ numDets│padding │  Detection[] │
│  2B    │  4B    │  2B    │  8B    │  variable    │
└────────┴────────┴────────┴────────┴──────────────┘
  magic:    0x5358 ('SX')
  frameId:  matches the frame that was processed
  numDets:  number of valid detections
  padding:  8 bytes reserved

Detection (12 bytes each, packed):
  ┌───────┬───────┬───────┬───────┬───────┬───────┐
  │  x1   │  y1   │  x2   │  y2   │ conf  │classId│
  │  f16  │  f16  │  f16  │  f16  │  f16  │ uint8 │
  └───────┴───────┴───────┴───────┴───────┴───────┘
  Coordinates: float16, model pixel space
  conf:        float16, [0, 1]
  classId:     uint8, 0=enemy 1=teammate
  (1 byte padding for alignment)
```

Total packet: 16 + numDets × 12 bytes. For 5 detections: 76 bytes.
This keeps the reply well under 100 bytes, minimizing network latency.

### 11.5 Testing Without the Host

If you need to test the Client inference pipeline without a running Host,
you can use the saved `client_test.bmp` as a test input. Write a small
test harness that:
1. Reads `client_test.bmp` into a BGRA buffer
2. Calls `trt.Infer(bgra, 0.25f)`
3. Prints detections

The BMP saved by the Client is exactly the same format the inference
module expects — no conversion needed.

### 11.6 Regenerating the TRT Engine

The `.engine` file is bound to the specific GPU and CUDA/TensorRT version.
If you upgrade drivers, CUDA, or TensorRT, you must regenerate:

```powershell
# Locate trtexec in TensorRT bin directory
& "C:\Program Files\NVIDIA\TensorRT-10.16.1.11...\bin\trtexec.exe" `
    --onnx=client\model\bf416.onnx `
    --saveEngine=client\model\bf416.engine `
    --fp16

# Validate
& "C:\Program Files\NVIDIA\TensorRT-10.16.1.11...\bin\trtexec.exe" `
    --loadEngine=client\model\bf416.engine
```

The ONNX file (`bf416.onnx`) is GPU-agnostic — keep it as the canonical
model artifact. Regenerate the engine on each deployment machine.

### 11.7 Memory Footprint

Client steady-state memory usage (approximate):

| Component | Memory |
|-----------|--------|
| ReassemblyBuffer data | 67 MB (worst-case pre-alloc) |
| ReassemblyBuffer mask | 6 KB |
| Decompress buffer | W×H×4 (e.g. 0.7 MB for 416²) |
| TRT engine (deserialized) | ~10–20 MB GPU |
| TRT input buffer (GPU) | W×H×3×4 (2.1 MB for 416²) |
| TRT output buffer (GPU) | 300×6×4 (7.2 KB) |
| Preprocess buffer (CPU) | W×H×3×4 (2.1 MB for 416²) |

**Total: ~90 MB system RAM + ~25 MB GPU VRAM.**

This is negligible for any machine with a discrete NVIDIA GPU.

### 11.8 Port Configuration Consistency

Both Host and Client default to port **8888**. When changing ports, make
sure both sides match. The Client binds to `INADDR_ANY` so it will accept
UDP from any source IP — no IP whitelisting is performed at the network
layer.
