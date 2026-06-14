# Synapse-X Client Specification

> **Target audience**: Host (capturing machine) developer.
> This document describes every detail of the Client-side UDP receive,
> out-of-order reassembly, LZ4 decompression, and verification pipeline.
> Use this alongside `HOST_SPEC.md` for joint debugging.

---

## 1. System Overview

```
┌── HOST (capturing machine) ──────────────────────────────────────┐
│                                                                   │
│  DXGI capture → LZ4 compress → fragment → UDP send               │
│                                                                   │
└───────┬───────────────────────────────────────────────────────────┘
        │  Ethernet (physically isolated, direct or switch)
        ▼
┌── CLIENT (this machine) ─────────────────────────────────────────┐
│                                                                   │
│  UDP recv (non-blocking drain)                                    │
│       │                                                           │
│       ▼ ReassemblyBuffer (out-of-order chunk reassembly)          │
│  Compressed LZ4 payload (1,638,400 bytes max)                     │
│       │                                                           │
│       ▼ LZ4_decompress_safe → verify == 1,638,400 bytes           │
│  BGRA pixel buffer  (640 × 640 × 4 = 1,638,400 bytes)            │
│       │                                                           │
│       ▼ [Future: TensorRT inference → bounding box → UDP reply]   │
│                                                                   │
└──────────────────────────────────────────────────────────────────┘
```

**Key numbers:**

| Parameter | Value |
|-----------|-------|
| Listen port | 8888 (configurable) |
| Socket mode | Non-blocking (`FIONBIO`) |
| Receive buffer | 256 KB socket buffer |
| Raw frame size | 1,638,400 bytes (640×640×4 BGRA) |
| Decompression | `LZ4_decompress_safe` (raw block, NOT frame format) |
| Max compressed size | 1,644,841 bytes (`LZ4_compressBound`) |
| Max chunks per frame | 1,175 (`ceil(1644841 / 1400)`) |

---

## 2. Module Architecture

```
client/
├── CLIENT_SPEC.md           ← this file
├── CMakeLists.txt           ← standalone CMake project
├── CMakePresets.json        ← VS 2026 x64 preset
├── include/
│   ├── ReassemblyBuffer.h   ← out-of-order reassembly engine
│   └── UdpReceiver.h        ← UDP recv + LZ4 decompress
└── src/
    ├── UdpReceiver.cpp      ← implementation
    └── main.cpp             ← verification & test loop
```

### Dependency graph

```
main.cpp
  └── UdpReceiver.h
        └── ReassemblyBuffer.h
              └── PacketHeader.h  (shared/)
```

### Build

```powershell
cd client
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
# Binary: build_x64/RelWithDebInfo/SynapseX_Client.exe
```

### Run

```powershell
.\SynapseX_Client.exe [port]    # default: 8888
```

---

## 3. ReassemblyBuffer — Out-of-Order Reassembly Engine

**File:** `client/include/ReassemblyBuffer.h`

### 3.1 Data Structure

```cpp
struct ReassemblyBuffer {
    uint32_t expectedFrameId;     // frameId currently being collected
    uint32_t totalSize;           // compressed size (from header.totalSize)
    uint16_t totalChunks;         // expected number of chunks
    uint16_t chunksReceived;      // unique chunks collected so far

    std::vector<bool>    receivedMask;  // bitmask: which chunks arrived
    std::vector<uint8_t> data;          // reassembly buffer (totalSize bytes)
};
```

### 3.2 Core Algorithm

This is the exact implementation of `HOST_SPEC.md` §3 algorithm, with the following
key behaviors:

#### Chunk placement — offset formula

```
offset = chunkIndex × MAX_PAYLOAD_SIZE  (= chunkIndex × 1400)
```

Each chunk is `memcpy`'d directly to this offset in the pre-allocated data buffer.
This works because all chunks except the last are exactly `MAX_PAYLOAD_SIZE` bytes.

Example: compressed frame of 3,000 bytes → 3 chunks:

| chunkIndex | offset | payloadSize |
|------------|--------|-------------|
| 0 | 0 | 1400 |
| 1 | 1400 | 1400 |
| 2 | 2800 | 200 |

#### Frame transition — the iron law

```
if (frameId > expectedFrameId):
    → discard old partial frame IMMEDIATELY (count as dropped)
    → start new frame

if (frameId < expectedFrameId):
    → drop (stale packet from old frame)

if (frameId == expectedFrameId):
    → insert chunk at correct offset
```

This guarantees zero head-of-line blocking. We **never** wait for missing
chunks from a previous frame — the moment a newer `frameId` arrives, the
old frame is irrevocably abandoned.

#### Duplicate detection

```cpp
if (receivedMask[chunkIndex] == true):
    → drop (duplicate packet, already received)
```

#### Completion check

```cpp
if (chunksReceived == totalChunks && totalChunks > 0):
    → frame is complete, ready for decompression
```

### 3.3 Memory Strategy

All buffers are **pre-allocated to worst-case size** in the constructor:

| Buffer | Pre-allocation | Rationale |
|--------|---------------|-----------|
| `data` | 1,644,841 bytes | `LZ4_compressBound(1638400)` |
| `receivedMask` | 1,175 bits (≈147 bytes) | `ceil(1644841 / 1400)` |

In steady state, `StartFrame()` only resizes if a frame's compressed size
exceeds the current capacity — which, with the worst-case pre-allocation,
**never** happens. This means **zero heap allocation in the receive hot path**.

### 3.4 Frame ID Wrap-Around

```cpp
inline bool IsNewerFrameId(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) > 0;
}
```

Uses signed-difference comparison. Correctly handles `uint32_t` wrap from
`0xFFFFFFFF` → `0x00000000`. At 170 Hz this wraps after ~290 days of
continuous operation — safe for any realistic session.

---

## 4. UdpReceiver — UDP Receive & Decompress Pipeline

**Files:** `client/include/UdpReceiver.h`, `client/src/UdpReceiver.cpp`

### 4.1 Lifecycle

```
Initialize(port)          bind socket, set non-blocking, pre-allocate buffers
       │
       ▼
┌── TryReceive() loop ──────────────────────────────────────┐
│                                                            │
│  1. recvfrom() in non-blocking drain loop                  │
│       │                                                    │
│       ▼                                                    │
│  2. ProcessDatagram() per packet:                          │
│     · Validate magic == 0x5358                             │
│     · Check frameId transition (newer/stale/same)           │
│     · InsertChunk() into ReassemblyBuffer                  │
│       │                                                    │
│       ▼                                                    │
│  3. If frame complete:                                     │
│     · LZ4_decompress_safe → verify == 1,638,400 bytes      │
│     · Update stats (frame count, drop rate, gap detection) │
│     · Reset buffer for next frame                          │
│                                                            │
└────────────────────────────────────────────────────────────┘
       │
       ▼
Cleanup()                  close socket, release WinSock
```

### 4.2 Socket Configuration

| Setting | Value | Rationale |
|---------|-------|-----------|
| Mode | `FIONBIO` (non-blocking) | Drain all queued datagrams without blocking |
| Receive buffer | 256 KB (`SO_RCVBUF`) | Absorb bursts from host's tight send loop |
| Bind address | `INADDR_ANY` (0.0.0.0) | Accept from any network interface |
| Protocol | `IPPROTO_UDP` | Raw UDP datagrams |

### 4.3 Per-Packet Processing (`ProcessDatagram`)

```
Input: raw UDP datagram (data, len)

1. If len < 16:
     → drop (too small for PacketHeader)

2. Cast first 16 bytes → PacketHeader*
   If header.magic != 0x5358:
     → drop (not our protocol)

3. Extract payload pointer:
     payload = &data[16]
     payloadSize = header.payloadSize

4. Safety check:
     If (16 + payloadSize) > len:
       → drop with warning (truncated datagram)

5. Frame transition:
     if !HasActiveFrame():
       → StartFrame(frameId, totalSize, totalChunks)
     elif IsNewerFrameId(frameId, expectedFrameId):
       → if old frame was incomplete: m_totalDropped++
       → StartFrame(frameId, totalSize, totalChunks)
     elif frameId != expectedFrameId:
       → drop (stale)

6. InsertChunk(chunkIndex, payload, payloadSize)
   If duplicate → drop

7. If IsComplete():
     → gap detection (see §5.1)
     → LZ4_decompress_safe → verify 1,638,400 bytes
     → if OK: m_totalFramesReceived++, return true
     → if FAIL: m_totalDropped++, reset
```

### 4.4 Decompression (Hot Path)

```cpp
int result = LZ4_decompress_safe(
    compressedData,       // reassembled from chunks
    outputBuffer,         // pre-allocated 1,638,400 bytes
    compressedSize,       // from header.totalSize
    1638400               // max output = 640×640×4
);

if (result != 1638400) {
    if (result < 0):  LZ4 corruption error
    if (result >= 0): size mismatch (corrupted frame)
    → discard frame, increment drop counter
}
```

**Critical invariant**: The output buffer (`m_decompressBuf`) is allocated
**once** in `Initialize()` and reused for every frame. No allocation in
the decompression path.

### 4.5 API

```cpp
class UdpReceiver {
public:
    bool Initialize(uint16_t port = 8888);
    void Cleanup();

    // Hot path — call at high frequency (≥ every 1 ms)
    // Returns true if a complete frame was decoded.
    // outFrame: 640×640×4 BGRA pixels
    // outFrameId: monotonic host frame counter
    bool TryReceive(std::vector<uint8_t>& outFrame, uint32_t& outFrameId);

    // Stats (cumulative, thread-safe for reads)
    uint64_t GetTotalFrames()  const;
    uint64_t GetTotalDropped() const;
    uint64_t GetTotalPackets() const;
    uint64_t GetTotalBytes()   const;
};
```

---

## 5. Drop Detection & Statistics

### 5.1 Two Categories of Drops

The Client tracks two distinct drop scenarios:

#### Type A: Abandoned partial frame

When a **newer** `frameId` arrives before the current frame is complete,
the old frame is discarded. This happens when:
- Network congestion causes tail-drop of some chunks
- Host sends frames faster than network can deliver

```
Frame N arrives, chunks 0, 1, 3 received (chunk 2 missing)
  → Frame N+1 arrives
  → Frame N is abandoned → m_totalDropped++
```

#### Type B: Frame ID gap (skipped frames)

When a complete frame is decoded, the Client checks whether the `frameId`
jumped by more than 1 since the last successfully decoded frame:

```cpp
int32_t gap = (frameId - m_lastDecodedFrameId) - 1;
if (gap > 0) {
    m_totalDropped += gap;  // frames we never even started
}
```

This catches frames where **all** chunks were lost (e.g., burst packet loss).
Without gap detection, these would silently disappear from stats.

### 5.2 Stats Output Format

Every second, the main loop prints:

```
---- per-second stats --------------------------------
  FPS:   170.3  |  frames:   170  |  dropped:     2  |  drop rate:   1.2%
  packets:  3400/s  |  throughput:   12.34 MB/s  |  total frames: 15230
```

| Field | Meaning |
|-------|---------|
| `FPS` | Successfully decoded frames per second |
| `frames` | Frames decoded in this 1-second window |
| `dropped` | Frames lost in this window (Type A + Type B) |
| `drop rate` | `dropped / (frames + dropped) × 100%` |
| `packets/s` | Raw UDP datagrams received per second |
| `throughput` | Network throughput in MB/s |
| `total frames` | Cumulative decoded frames since start |

**Important for Host developers**: If `drop rate` is consistently > 0%
under light network load, check:
1. Socket buffer sizing on both ends
2. Network cable / switch quality
3. Host `sendto()` error handling (does the host continue after a failed send?)

---

## 6. Verification — BMP Output

### 6.1 Trigger

When the **10th complete frame** is successfully decoded, `main.cpp`
automatically writes:

```
client_test.bmp     (640×640, 32-bit BGRA, top-down DIB)
```

This BMP is **byte-for-byte compatible** with the Host's `test_roi.bmp`.
You can diff them pixel-for-pixel to verify the full pipeline.

### 6.2 BMP Format Details

| Field | Value | Notes |
|-------|-------|-------|
| `bfType` | `0x4D42` ('BM') | Standard BMP magic |
| `biWidth` | 640 | |
| `biHeight` | **-640** | Negative = top-down DIB (no flip) |
| `biBitCount` | 32 | BGRA, 8 bits per channel |
| `biCompression` | `BI_RGB` (0) | Uncompressed |
| Row order | Top-to-bottom | First row in file = top of screen |
| Channel order | B, G, R, A | Matches DXGI_FORMAT_B8G8R8A8_UNORM |

### 6.3 Manual Verification

The Client also prints the first 4 pixels after BMP save:

```
[VERIFY] First 4 pixels (B,G,R,A): [ 31, 31, 31,255] [ 31, 31, 31,255] ...
```

Compare these against the Host's `test_roi.bmp` first 4 pixels.

---

## 7. Joint Debugging Guide

### 7.1 1-to-1 Loopback Test (Single Machine)

```powershell
# Terminal 1 — start Client FIRST
cd client\build_x64\RelWithDebInfo
.\SynapseX_Client.exe 8888

# Terminal 2 — start Host
cd host\build_x64\RelWithDebInfo
.\SynapseX_Host.exe 127.0.0.1 8888
```

Expected Client output within a few seconds:
```
[VERIFY] Frame #10 (Host frameId=9) saved as 'client_test.bmp'
---- per-second stats --------------------------------
  FPS:    25.0  |  frames:    25  |  dropped:     0  |  drop rate:   0.0%
```

### 7.2 Physical Isolation Test (Two Machines)

```
Client machine                    Host machine
├─ IP: (any)                      ├─ IP: 192.168.1.100 (example)
├─ Run: SynapseX_Client.exe 8888  ├─ Run: SynapseX_Host.exe 192.168.1.101 8888
└─ Listens on 0.0.0.0:8888       └─ Sends to Client's IP
```

### 7.3 Common Issues

| Symptom | Likely Cause | Check |
|---------|-------------|-------|
| Client prints nothing | Host not sending, or firewall blocking UDP | `netstat -ano \| findstr 8888` on Client |
| `drop rate` > 0% on loopback | Rare (loopback is reliable). Check socket buffers. | Bump `SO_RCVBUF` / `SO_SNDBUF` to 512 KB |
| `LZ4_decompress_safe ERROR` | Corrupted payload. Packet truncation or bit-flip? | Compare `compressed=` value vs Host's actual compressed size |
| `LZ4 size mismatch: got X, expected 1638400` | Frame incorrectly reassembled. Chunks placed at wrong offsets? | Verify `chunkIndex * 1400` offset formula matches on both ends |
| BMP looks garbled | Row stride mismatch | Verify `biHeight = -640` and no extra row padding |
| BMP is all black | Host captured black frame or zero data on wire | Check Host `test_roi.bmp` first |

### 7.4 Diagnostic Logging

Both Host and Client print diagnostics to `stderr`. Key log lines:

**Client startup:**
```
[UdpReceiver] Ready — listening on 0.0.0.0:8888, non-blocking, recv buffer 256 KB
```

**Packet received (suppressed in release — uncomment in code if needed):**
```
// To enable: uncomment fprintf in ProcessDatagram
[UdpReceiver] frameId=42 chunk=3/25 payload=1400 totalSize=28500
```

**Frame complete:**
```
// Implicit — stats line appears next second
---- per-second stats --------------------------------
  FPS:   170.3  |  frames:   170  |  dropped:     0  |  drop rate:   0.0%
```

---

## 8. Future: TensorRT Inference Pipeline

The CMake build already detects CUDA 13.1 and TensorRT. When the inference
module is added:

```
Expected directory layout:
  client/src/TrtInference.cpp    ← TensorRT engine loading + inference
  client/src/TrtInference.cu     ← CUDA pre/post-processing kernels

Expected data flow:
  BGRA 640² → GPU upload → normalization → TRT inference
  → bounding box coordinates → UDP reply to Host

Host ← Client response protocol (to be defined):
  Byte offset  0       4       8      16      24
            ┌────────┬────────┬────────┬────────┐
            │ magic  │frameId │  x, y  │  w, h  │  (32 bytes total)
            └────────┴────────┴────────┴────────┘
```

Current CMake scaffolding (uncomment when ready):

```cmake
# In client/CMakeLists.txt — search for "TensorRT 推理模块"
# Uncomment the add_library(SynapseX_Inference ...) block
# and the target_link_libraries addition
```

---

## 9. File Index

| File | Purpose | Lines |
|------|---------|-------|
| `client/CMakeLists.txt` | Build config: LZ4, CUDA 13.1, TensorRT, ws2_32 | ~200 |
| `client/CMakePresets.json` | VS 2026 x64 preset | ~35 |
| `client/include/ReassemblyBuffer.h` | Reassembly engine (header-only) | ~125 |
| `client/include/UdpReceiver.h` | UDP receiver interface | ~115 |
| `client/src/UdpReceiver.cpp` | Receive, reassemble, decompress | ~295 |
| `client/src/main.cpp` | Verification loop, stats, BMP writer | ~315 |
| `client/CLIENT_SPEC.md` | This file | — |

---

## 10. Quick Reference Card

```
┌─────────────────────────────────────────────────────────────────┐
│                    Synapse-X CLIENT — Quick Reference            │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  BUILD:    cd client && cmake --preset windows-x64               │
│            cmake --build build_x64 --config RelWithDebInfo       │
│                                                                  │
│  RUN:      .\build_x64\RelWithDebInfo\SynapseX_Client.exe [port] │
│                                                                  │
│  LISTEN:   UDP 0.0.0.0:8888 (default)                           │
│                                                                  │
│  FORMAT:   640×640 BGRA, 1,638,400 bytes per frame               │
│                                                                  │
│  PROTOCOL: magic=0x5358, 16-byte PacketHeader per chunk          │
│            chunk offset = chunkIndex × 1400                      │
│                                                                  │
│  VERIFY:   10th frame → client_test.bmp                          │
│            Compare with Host's test_roi.bmp                      │
│                                                                  │
│  STATS:    Per-second: FPS, drop rate, throughput                │
│            Printed to stderr                                     │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```
