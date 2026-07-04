# Synapse-X

**超低延迟双机AI物理隔离视觉推理管线，集成实时自瞄辅助。**

主机以 170Hz 截取游戏画面，LZ4 压缩后通过物理隔离网线发送给副机；副机运行 TensorRT YOLO 推理，返回检测坐标；主机通过 ddll64.dll 驱动鼠标自瞄。所有参数可通过网页面板实时调参。

---

## 架构

```
┌── 主机（游戏机）──────────────────────────────────────────────┐
│                                                                │
│  GPU (DXGI 截屏) → LZ4 → UDP :8888 ──────────┐                │
│                                               │                │
│  UDP :8889 ←──────────────────────────────┐   │                │
│    │  ReplyHeader + DetectionRaw[]         │   │                │
│    ▼                                      │   │                │
│  目标选择 → MouseController (ddll64.dll)   │   │                │
│    自瞄（头/身、动态Kp、可调参）            │   │                │
│                                               │                │
│  HttpTuner (:9999) ← 网页面板（手机/平板）    │                │
│    实时参数调节                               │                │
└───────────────────────────────────────┼───────┼────────────────┘
                                        │       │
                            物理隔离以太网直连
                                        │       │
┌── 副机（推理机）──────────────────────┼───────┼────────────────┐
│                                       │       │                │
│  UDP :8888 收包 → 乱序重组 → LZ4 解压 ┘       │                │
│                                                │                │
│  TensorRT 推理 (YOLO, 416×416 FP16) ───────────┘                │
│    UDP :8889 回传 (ReplyHeader + DetectionRaw[])               │
└─────────────────────────────────────────────────────────────────┘
```

| 组件 | 机器 | 职责 |
|------|------|------|
| `host/` | 游戏机 | 截屏、压缩、发送、接收回复、自瞄、网页调参 |
| `client/` | 推理机 | 收包、乱序重组、解压、TensorRT 推理、回传坐标 |
| `shared/` | 两端共享 | 通信协议头 (PacketHeader, ReplyPacket) |

---

## 功能特性

- **170Hz 固定截屏** — DXGI Desktop Duplication，GPU 端中心 ROI 裁剪，线程绑核 P-Core
- **LZ4 压缩** — `LZ4_compress_fast(accel=5)`，压缩比约 3–30%，游戏负载下 <0.5ms
- **UDP 分包** — 24 字节 PacketHeader + ≤1400 字节载荷，非阻塞 4MB 缓冲区，MTU 安全
- **可配置 ROI** — CLI: `416×416`、`640×640`，或任意 64–4096 像素
- **TensorRT 推理** — YOLO FP16，RTX GPU，稳定约 1.5–1.8ms
- **双向 UDP** — 主机→副机 :8888（帧数据），副机→主机 :8889（检测结果）
- **PD 自瞄** — 动态 Kp（远距离保守、贴脸磁吸）+ Kd 阻尼 + 亚像素累加器 + 2 帧延迟补偿
- **网页调参面板** — `http://<主机IP>:9999`，手机/平板可访问，实时生效
- **抗退化** — 核心绑定 + LZ4 动态加速 + UDP 非阻塞，游戏负载下管线不崩
- **自动恢复** — DXGI 丢失自动重建全链路

---

## 快速开始

### 环境要求

- Windows 10/11 x64, Visual Studio 2026, CMake 3.28+
- 副机需 NVIDIA GPU (CUDA 13.1, TensorRT 10.16)
- 物理隔离以太网直连：主机 `192.168.100.1` ↔ 副机 `192.168.100.2`

### 构建

```powershell
# 主机（游戏机）
cd host
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo

# 副机（推理机）
cd client
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
```

### 运行

```powershell
# 副机 — 先启动
.\SynapseX_Client.exe 8888 ..\..\model\bf416.engine 192.168.100.1

# 主机 — 管理员权限运行（鼠标控制需要）
.\SynapseX_Host.exe 192.168.100.2 8888 416 416

# 网页调参面板
http://192.168.100.1:9999
```

### 命令行参数

```
SynapseX_Host.exe [目标IP] [端口] [宽] [高]
  默认值: 192.168.100.2  8888   416   416

SynapseX_Client.exe [端口] [引擎路径] [主机IP] [--save]
  默认值: 8888  bf416.engine  192.168.100.1
```

---

## 关键指标

| 指标 | 数值 |
|------|------|
| 主机截屏帧率 | **170 Hz**（5.88ms 固定节奏，线程绑核 P-Core，TIME_CRITICAL） |
| 主机管线延迟 | **~0.35 ms**（空闲和游戏中） |
| 副机推理延迟 | **~1.5–1.8 ms** 稳定（GPU 预处理 NVRTC，P-State 锁定） |
| 端到端延迟 | **~2 ms** 截屏 → 推理 → 自瞄 |
| 网络每帧数据量 | **30–400 KB**（取决于画面内容，LZ4 accel=5） |
| UDP 数据报 | ≤1424 字节（24B 头 + ≤1400B 载荷），非阻塞 4MB 缓冲 |
| 自瞄控制器 | 动态 Kp + Kd 阻尼 + 亚像素累加器 + 2 帧延迟补偿 |
| ROI 范围 | 64×64 到 4096×4096，运行时配置 |

---

## 目录结构

```
Synapse-X/
├── README.md                        ← 本文件
├── .gitignore
├── CMakeLists.txt
│
├── shared/include/
│   ├── PacketHeader.h               主机→副机通信协议 (24B)
│   └── ReplyPacket.h                副机→主机回传协议 (16B)
│
├── host/
│   ├── include/
│   │   ├── DxgiCapturer.h           GPU 截屏模块
│   │   ├── Lz4Compressor.h          LZ4 块压缩模块
│   │   ├── UdpSender.h              UDP 分包发送模块
│   │   ├── UdpReplyReceiver.h       UDP 回复监听 + 坐标映射
│   │   ├── MouseController.h        动态Kp控制器 + 自瞄配置
│   │   └── HttpTuner.h              网页调参服务器
│   ├── src/                         实现文件 + main.cpp
│   ├── web/index.html               前端面板（从磁盘读取，无需重编译）
│   ├── test/test_bmp.cpp            独立截屏测试
│   ├── mousedll/ddll64.dll          鼠标控制 DLL（已提交仓库）
│   ├── CMakeLists.txt
│   ├── CMakePresets.json
│   └── HOST_SPEC.md                 主机完整规格说明
│
├── client/
│   ├── include/
│   │   ├── ReassemblyBuffer.h       乱序重组引擎
│   │   ├── UdpReceiver.h            UDP 收包 + 解压
│   │   └── TrtInference.h           TensorRT 推理封装
│   ├── src/                         实现文件 + main.cpp
│   ├── model/engine/                模型引擎文件
│   ├── CMakeLists.txt
│   └── CLIENT_SPEC.md               副机完整规格说明
│
└── thirdparty/
    ├── lz4-1.10.0/                   LZ4 源码（直接编译）
    └── cpp-httplib-0.47.0/           HTTP 服务器（单头文件）
```

---

## 规格文档

| 文档 | 内容 |
|------|------|
| [host/HOST_SPEC.md](host/HOST_SPEC.md) | 管线、模块、协议、CLI、性能优化、活跃问题 |
| [host/MOUSE_CONTROL_SPEC.md](host/MOUSE_CONTROL_SPEC.md) | 动态Kp控制器、亚像素累加器、延迟补偿、目标选择、调参指南 |
| [client/CLIENT_SPEC.md](client/CLIENT_SPEC.md) | 乱序重组、推理管线、回传协议、GPU 性能问题 |

---

## 排错指南

| 现象 | 解决方案 |
|------|---------|
| 游戏中截图全黑 | 游戏改为**无边框窗口**模式 |
| `[MouseCtrl] OpenDevice FAILED` | 以**管理员权限**运行 |
| 无 `[Reply]` 输出 | 检查防火墙 UDP 8889；确认副机发送到正确的主机 IP |
| 网页面板打不开 | Ctrl+Shift+R **强制刷新**浏览器；确认 `http://<主机IP>:9999` |
| 自瞄过冲/震荡 | 增加 Kd（阻尼）；增大 kpDecay（缩小磁吸范围） |
| 自瞄太慢 | 增加 Kp（基础跟枪速度）；加大 kpMax（贴脸磁吸） |
| 游戏中管线延迟飙升 | 确认线程绑核（core 2）；UDP 非阻塞已启用；LZ4 accel=5 |
| 副机推理抖动 17–27ms | 锁定 GPU 频率；预热 TRT 引擎；参见 CLIENT_SPEC |
| `[UdpSender] packet dropped` | 极端负载下正常——170Hz 丢一帧肉眼不可见 |
