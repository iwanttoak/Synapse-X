# Synapse-X — REASONIX.md

## Stack

- **Language** C++17, MSVC (VS 18 2026 / v143), Windows SDK 10.0.26100
- **Build** CMake 3.28+, `cmake --preset windows-x64`
- **Host deps** D3D11, DXGI, Winsock2, WinMM, LZ4 1.10.0 (source in `thirdparty/`), cpp-httplib 0.47.0 (header-only)
- **Client deps** CUDA 13.1/13.2, TensorRT 10.16.1.11 (hardcoded path), NVRTC (runtime JIT — avoids VS 2026 + nvcc incompatibility)
- **Logging** spdlog 1.17.0 via `SX_LOG_*` macros (`shared/include/Log.h`)

## Layout

| Path | Contents |
|------|----------|
| `host/` | Gaming PC — DXGI capture → LZ4 → UDP send, PD aim, HTTP tuner (:9999) |
| `client/` | Inference PC — UDP recv → reassembly → TensorRT YOLO FP16 → reply |
| `shared/include/` | Wire protocol: `PacketHeader.h` (24B, magic 0x5358), `ReplyPacket.h` (16B + DetectionRaw[]), `Log.h` |
| `thirdparty/` | Vendored `lz4-1.10.0/`, `cpp-httplib-0.47.0/`, `spdlog-1.17.0/` |

## Commands

```powershell
# Host (gaming PC)
cd host && cmake --preset windows-x64 && cmake --build build_x64 --config RelWithDebInfo
# → SynapseX_Host.exe + SynapseX_Host_TestBmp.exe

# Client (inference PC)
cd client && cmake --preset windows-x64 && cmake --build build_x64 --config RelWithDebInfo
# → SynapseX_Client.exe

# Client inference test
cd client/test && cmake --preset windows-x64 && cmake --build build_x64 --config RelWithDebInfo
# → test_infer.exe (links WIC + COM)
```

No lint/format/typecheck setup (`.clang-format` only in thirdparty/).

## Conventions

- **Namespace:** `SynapseX` (log: `SynapseX::Log`)
- **Naming:** PascalCase classes/methods (`DxgiCapturer`), `m_` members, `k` constants, snake_case locals
- **Headers:** `#pragma once`; `.h`/`.cpp` pair per class
- **Wire structs:** `#pragma pack(push, 1)` + immediate `static_assert(sizeof(...)==N)`
- **Comments:** Chinese; code identifiers in English
- **Log init:** `SynapseX::Log::Initialize("appname")` — console + rotating file sink
- **Hotkey:** `GetAsyncKeyState` edge-triggered — PageUp (enable aim), PageDown (disable)

## Watch out for

- **Host and Client build independently on separate machines.** Root `CMakeLists.txt` is IDE-only; real build must `cd host` or `cd client`.
- **TensorRT paths hardcoded** in `client/CMakeLists.txt` — must edit for different TRT versions.
- **`.engine` files** are GPU + TRT-version-specific. Regenerate with `trtexec --fp16` if hardware changes.
- **`ddll64.dll`** committed at `host/mousedll/`; CMake auto-copies to build output. Missing → aim disabled at runtime.
- **CUDA 13.1 nvcc is incompatible with VS 2026.** Client uses NVRTC runtime compilation (`CudaPreprocess.cpp`) — no `.cu` files.
- **LIFO slot (size 1)** on client: consumer busy → producer overwrites + increments `drops`. By design.
- **Host 170Hz** via `sleep_until(nextTick)`. Falls behind → `nextTick` resets to `now`.
- **Run host as Administrator** — `ddll64.dll` requires elevation.
