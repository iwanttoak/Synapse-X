# Synapse-X 架构重构计划：鼠标控制拆到副机

## 一、需求

1. **主机只做截图+发图** — 不再参与任何推理、控制、网络接收逻辑
2. **全部智能逻辑搬到副机** — 推理、目标选择、PD 控制、鼠标移动都在副机完成
3. **使用 MAKCU 硬件直接控制鼠标** — MAKCU 是 USB HID 硬件鼠标控制器，插在副机上，副机通过 `makcu::mouseMove()` 发送串口命令，MAKCU 以物理鼠标身份向主机注入移动
4. **HttpTuner 搬到副机** — 配置面板在副机 :9999 端口提供
5. **同步方法不变** — 不引入新的配置同步协议，配置直接在副机本地管理
6. **协议只增加一个 flags 字节** — 用于 Host 告诉 Client 热键状态

## 二、为什么这么做

### 现状问题

当前架构：

```
Host (游戏PC)                                Client (推理PC)
DXGI → LZ4 → UDP :8888 ─────────────── UDP接收 → 重组 → LZ4解压 → TRT推理
                    ← UDP :8889 ──────────────── 发送检测结果
UdpReplyReceiver → 目标选择 → PD控制 → MoveR(ddll64.dll)
HttpTuner :9999
```

主机管线每 tick（~5.88ms）执行：采集 → 压缩 → 发送 → **接收回复** → **目标选择** → **PD 控制** → **鼠标移动**。主机需要同时做网络发送、网络接收、游戏逻辑、控制算法，虽然延迟很小（~0.35ms），但架构上不干净。

### 重构后的优势

1. **主机减负到极致** — 主循环只需采集→压缩→发送，没有任何网络接收、控制算法、配置管理，CPU 占用近乎零
2. **副机全权控制** — 推理、目标选择、PD 算法、鼠标移动全部在副机，逻辑内聚
3. **硬件级鼠标注入** — MAKCU 是物理 USB HID 设备，不是内核驱动，不会被反作弊检测（相比 `ddll64.dll` 的 kernel-level 钩子）
4. **零网络往返** — 当前架构中检测结果需从副机发回主机再执行 PD，多一次 UDP 往返；重构后 PD 直接在副机执行，结果直接通过 MAKCU USB 串口输出（~0.04ms），不再依赖网络
5. **配置本地化** — HttpTuner 直接在副机 :9999 提供服务，不需要跨机同步 AimConfig
6. **安全性** — 副机只发 UDP 不接收（除初始帧数据外），主机完全不需要开入站端口

## 三、目标架构

```
Host (游戏PC)                           Client (推理PC) — 全部智能
┌────────────────────┐                 ┌─────────────────────────────────────┐
│ DXGI → LZ4 → UDP   │                │ UDP接收 → 重组 → LZ4解压 → TRT推理   │
│ (flags=热键状态)    │                │                                      │
│                     │                │ PdController → (dx, dy)              │
│ 无网络接收           │                │      ↓                              │
│ 无控制逻辑           │                │ MouseDevice(makcu)                  │
│ 无配置管理           │                │      ↓ USB 串口                     │
│ 无 HttpTuner        │                │  ┌──────────┐                       │
└────────────────────┘                 │  │ MAKCU硬件 │─── USB HID ──→ 游戏PC鼠标
                                       │  └──────────┘                       │
                                       │                                      │
                                       │  HttpTuner :9999 (web/ 面板)         │
                                       │  ↑ 热键状态来自 PacketHeader.flags    │
                                       └─────────────────────────────────────┘
```

### 数据流

```
每 tick (~5.88ms @ 170Hz):

Host:
  1. GetAsyncKeyState(PageUp/PageDown) → flags
  2. DxgiCapturer::CaptureFrame()
  3. Lz4Compressor::Compress() (新帧时)
  4. UdpSender::SendCompressedFrame(data, frameId, roiW, roiH, modelId, flags)

Client (Producer 线程, core 0):
  1. UdpReceiver::TryReceive() → 重组 → LZ4 解压
  2. LIFO FrameSlot 推送 (覆盖旧帧)

Client (Consumer 线程, core 1):
  1. LIFO FrameSlot 弹出
  2. TrtInference::Infer() → std::vector<Detection>
  3. PdController::Run(dets, config, flags)
     ├─ 归一化为 AimPoint[] (8 个 model 的 switch-case)
     ├─ 空间锁 (kKeepLockRadius=80, kMaxLostFrames=5)
     ├─ 动态 Kp PD 算法
     ├─ 亚像素累加器
     └─ 延迟补偿 (2 帧环形缓冲区)
  4. if hasTarget && aimEnabled:
       MouseDevice::Move(dx, dy)
       ├─ makcu::mouseMove(dx, dy) → USB 串口 → MAKCU 硬件 → HID → 主机
```

## 四、逐步实施计划

### Phase 1: 协议层 — PacketHeader 加 flags

**文件: `shared/include/PacketHeader.h`**

```cpp
// ── flags 定义 ──
enum FrameFlags : uint8_t {
    FF_AIM_ENABLED = 0x01,   // Host 热键状态: PageUp=1, PageDown=0
};

struct PacketHeader {
    uint16_t magic        = PROTOCOL_MAGIC;  // 0
    uint32_t frameId      = 0;               // 2
    uint16_t totalChunks  = 0;               // 6
    uint16_t chunkIndex   = 0;               // 8
    uint32_t totalSize    = 0;               // 10
    uint16_t payloadSize  = 0;               // 14
    uint16_t width        = 0;               // 16
    uint16_t height       = 0;               // 18
    uint8_t  modelId      = 0;               // 20  ← 保留（副机本地管理）
    uint8_t  flags        = 0;               // 21  ← NEW: FrameFlags
    uint8_t  padding[2]   = {0};             // 22-23
};
static_assert(sizeof(PacketHeader) == 24);  // 不变
```

### Phase 2: Host 源文件清理

删除以下引用，源文件在 Phase 11 删除：
- `host/src/main.cpp` — 去掉 `#include "UdpReplyReceiver.h"`, `"MouseController.h"`, `"HttpTuner.h"`
- `host/include/UdpReplyReceiver.h`
- `host/src/UdpReplyReceiver.cpp`
- `host/include/MouseController.h`
- `host/src/MouseController.cpp`
- `host/include/HttpTuner.h`
- `host/src/HttpTuner.cpp`

### Phase 3: Host main.cpp 精简

从 ~548 行减到 ~220 行，保留：

```
初始化: DxgiCapturer → Lz4Compressor → UdpSender
         timeBeginPeriod, 核心绑定, 优先级

主循环:
  while (g_running):
    1. GetAsyncKeyState(PageUp/PageDown) → flags
    2. CaptureFrame()
    3. if 新帧: Compress() → 更新缓存
    4. SendCompressedFrame(data, size, frameId, roiW, roiH, modelId, flags)
    5. 统计日志 (直接 SX_LOG_DEBUG, 不用 HttpTuner)
    6. sleep_until(nextTick)

清理: timeEndPeriod, Cleanup
```

**删除内容:**
- `UdpReplyReceiver` 初始化和 `ReceiveReplies()` 调用
- `MouseController` 加载和 `AimAtTarget()` 调用
- `HttpTuner` 启动、`GetConfig()`、`UpdateStats()`、`UpdateTarget()`
- 整个 `AimPoint` 结构体
- 整个 `isLocked/lockedTargetX/lockedTargetY/lostFrames/lockedPriority` 状态
- 整个 `switch(modelId)` 8 个 case 目标归一化（~100 行）
- `PerfStats` 保留但简化（capture + compress + send 即可）
- 删 `tuner.GetConfig().modelId` → 改为直接从参数传入 `frameId`

**保留统计:** 每秒打印采集帧率、压缩耗时、发送帧率（纯控制台，不推 HttpTuner）

### Phase 4: UdpSender 加 flags 参数

**文件: `host/include/UdpSender.h` + `host/src/UdpSender.cpp`**

```cpp
// 新增 uint8_t flags 参数
bool SendCompressedFrame(const uint8_t* compressedData,
                         uint32_t totalSize,
                         uint32_t frameId,
                         uint16_t width,
                         uint16_t height,
                         uint8_t  modelId,
                         uint8_t  flags);       // ← NEW

// 实现中:
header->flags = flags;  // 写入 PacketHeader
```

### Phase 5: Host CMakeLists.txt 清理

**文件: `host/CMakeLists.txt`**

```cmake
# 源文件列表精简
set(HOST_SOURCES
    src/DxgiCapturer.cpp
    src/Lz4Compressor.cpp
    src/UdpSender.cpp
    src/main.cpp
)

# 链接库精简
set(HOST_LIBS
    SynapseX_Shared
    lz4_static
    d3d11
    dxgi
    ws2_32
    winmm
)

# 删除:
# - cpp-httplib include (已无 HttpTuner)
# - ddll64.dll POST_BUILD copy (已无 MouseController)
# - web/ POST_BUILD copy (已无 HttpTuner)

# 保留:
# - SynapseX_Host_TestBmp 测试程序
```

### Phase 6: Client 新增 PdController（目标选择 + PD 算法）

**新文件: `client/include/PdController.h`**

```cpp
#pragma once
#include <cstdint>
#include <vector>
#include <cmath>

namespace SynapseX {

// ── AimConfig — 从 MouseController.h 搬来 ──
struct AimConfig {
    float Kp              = 0.30f;
    float Kd              = 0.05f;
    float aimRange        = 200.0f;
    float minConfidence   = 0.25f;
    float deltaHeadConfidence = 0.40f;
    int   aimPoint        = 0;
    float headOffset      = 0.20f;
    int   nativeW         = 3840;
    int   nativeH         = 2160;
    int   gameW           = 3840;
    int   gameH           = 2160;
    uint8_t modelId       = 0;
    float kpMax           = 0.75f;
    float kpDecay         = 0.05f;
    int   classFilter     = 2;
};

// ── PD 结果 ──
struct PdResult {
    int   dx = 0, dy = 0;   // 鼠标增量
    float targetX = 0;       // 锁定的目标屏幕坐标（面板显示用）
    float targetY = 0;
    float distance = 0;
    bool  hasTarget = false;
    int   lockedPriority = 0;
};

// ── PdController ──
class PdController {
public:
    PdController();

    // 核心接口：输入检测结果，输出鼠标增量
    PdResult Run(const Detection dets[], int numDets,
                 const AimConfig& cfg, bool aimEnabled,
                 int screenW, int screenH);

    // 配置更新
    void SetConfig(const AimConfig& cfg);
    const AimConfig& GetConfig() const;

    // 重置状态（目标丢失时）
    void Reset();

private:
    // 从 host/src/main.cpp 搬来的目标归一化逻辑
    struct AimPoint {
        float cx, cy;
        int   priority;
        float distance;
    };
    std::vector<AimPoint> NormalizeDetections(
        const Detection dets[], int numDets,
        const AimConfig& cfg, int screenW, int screenH);

    // 从 MouseController.cpp 搬来的 PD 算法
    struct PdState {
        float prevErrorX = 0, prevErrorY = 0;
        float residualX = 0, residualY = 0;
        struct SentMove { int dx, dy; };
        SentMove sentHistory[2] = {};
        int sentWriteIdx = 0, sentCount = 0;
    };
    PdState m_state;

    // 空间锁状态
    bool  m_isLocked = false;
    float m_lockedTargetX = 0, m_lockedTargetY = 0;
    int   m_lostFrames = 0, m_lockedPriority = 0;

    AimConfig m_config;
    static constexpr float kKeepLockRadius = 80.0f;
    static constexpr int   kMaxLostFrames  = 5;
};

} // namespace SynapseX
```

**新文件: `client/src/PdController.cpp`**

从 `host/src/main.cpp` 搬来：
- `NormalizeDetections()` — 8 个 model 的 switch-case，置信度过滤、头部/身体瞄准点选择

从 `MouseController.cpp` 搬来：
- `AimAtTarget()` → 改名 `Run()` — 动态 Kp、D 项、亚像素累加器、延迟补偿

新增逻辑：
- `SelectTarget()` — 空间锁：锁定维持、丢失超时、自动升级

### Phase 7: Client 新增 MouseDevice（MAKCU 封装）

**新文件: `client/include/MouseDevice.h`**

```cpp
#pragma once
#include <makcu.h>

namespace SynapseX {

class MouseDevice {
public:
    MouseDevice() = default;
    ~MouseDevice();

    bool Connect();          // findFirstDevice → connect(highSpeed=true)
    void Move(int dx, int dy); // makcu::mouseMove(dx, dy), ~0.04ms
    void Click();            // makcu::click(LEFT), 支持开火
    void Disconnect();
    bool IsConnected() const;

private:
    makcu::Device m_device;
    bool m_connected = false;
};

} // namespace SynapseX
```

### Phase 8: Client 搬入 HttpTuner + web/

- 复制 `host/include/HttpTuner.h` → `client/include/HttpTuner.h`（微调统计字段）
- 复制 `host/src/HttpTuner.cpp` → `client/src/HttpTuner.cpp`（改统计为目标端统计）
- 复制 `host/web/` → `client/web/`

**TuningState 适配副机：**

```cpp
struct TuningState {
    AimConfig config;
    bool      aimEnabled = true;

    // 副机统计
    double receiveFps  = 0.0;   // 接收帧率
    double inferFps    = 0.0;   // 推理帧率
    double inferMs     = 0.0;   // 推理平均耗时
    double pipelineMs  = 0.0;   // 端到端延迟
    int    freshFrames = 0;
    int    droppedFrames = 0;

    // 目标信息（来自 PdController）
    struct TargetInfo {
        bool   active     = false;
        float  screenX    = 0;
        float  screenY    = 0;
        float  confidence = 0;
        float  distance   = 0;
        int    dx         = 0;  // 当前帧鼠标增量
        int    dy         = 0;
    };
    TargetInfo target;

    // 热键状态（来自 PacketHeader.flags）
    bool hostAimEnabled = true;
};
```

### Phase 9: Client main.cpp 重写

**变更点：**

删除：
- `#include "UdpReplySender.h"` 和相关代码
- `ConsumerCtx::replySender` / `replyReady`
- SendReplies() 调用

新增：
- `#include "PdController.h"`
- `#include "MouseDevice.h"`
- `#include "HttpTuner.h"`
- FrameSlot 增加 `uint8_t flags` 字段（从 UdpReceiver 传入）

**Consumer 线程新流程：**

```cpp
// 推理完成后
if (!dets.empty()) {
    PdResult result = pdController.Run(
        dets.data(), (int)dets.size(),
        config, aimEnabled,
        modelW, modelH);

    if (result.hasTarget && aimEnabled) {
        mouseDevice.Move(result.dx, result.dy);
        tuner.UpdateTarget(result.targetX, result.targetY,
                           1.0f, result.distance,
                           result.dx, result.dy);
    }
}

// 不再需要 replySender->SendReplies()
```

**FrameSlot 扩充：**

```cpp
struct FrameSlot {
    std::mutex              mtx;
    std::condition_variable cv;
    std::vector<uint8_t>    data;
    uint32_t                frameId  = 0;
    uint16_t                roiW     = 0;
    uint16_t                roiH     = 0;
    uint8_t                 flags    = 0;  // ← NEW: Host 热键状态
    bool                    hasNew   = false;
    uint64_t                drops    = 0;
};
```

**UdpReceiver 新增接口 `uint8_t GetLastFlags() const`**，从解包后的 PacketHeader 提取。

### Phase 10: Client CMakeLists.txt

```cmake
# 新源文件
set(CLIENT_SOURCES
    src/UdpReceiver.cpp
    src/PdController.cpp      # ← NEW
    src/MouseDevice.cpp        # ← NEW
    src/HttpTuner.cpp          # ← NEW (从 host 搬来)
    src/TrtInference.cpp
    src/CudaPreprocess.cpp
    src/main.cpp
)
# 删除 src/UdpReplySender.cpp

# HttpTuner — cpp-httplib 头文件
target_include_directories(SynapseX_Client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/cpp-httplib-0.47.0
)

# MAKCU 鼠标控制器
find_package(makcu-cpp REQUIRED)
target_link_libraries(SynapseX_Client PRIVATE makcu::makcu-cpp)

# 复制 web/ 面板到输出目录
add_custom_command(TARGET SynapseX_Client POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/web"
        "$<TARGET_FILE_DIR:SynapseX_Client>/web"
    COMMENT "Copying web/ to output directory"
)
```

### Phase 11: 删除废弃文件

```
host/include/UdpReplyReceiver.h
host/src/UdpReplyReceiver.cpp
host/include/MouseController.h
host/src/MouseController.cpp
host/include/HttpTuner.h
host/src/HttpTuner.cpp
host/web/
host/mousedll/

client/include/UdpReplySender.h
client/src/UdpReplySender.cpp
```

`shared/include/ReplyPacket.h` 保留（`DetectionRaw` 内部仍用于 `Detection` 格式）。

### Phase 12: 构建验证

```powershell
# Host
cd host
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
# 输出: SynapseX_Host.exe, SynapseX_Host_TestBmp.exe

# Client
cd client
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
# 输出: SynapseX_Client.exe

# Client inference test
cd client\test
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
# 输出: test_infer.exe
```

验证点：
- Host 能正常截图压缩发送，无链接错误
- Client 能正常接收推理，无链接错误
- `makcu-cpp` 被正确链接
- `HttpTuner` + `cpp-httplib` 在 Client 可编译
- `web/` 被复制到 Client 输出目录
- Host 输出目录无 `ddll64.dll`、`web/` 等废弃文件

## 五、文件变更汇总

| 操作 | 文件 | 说明 |
|------|------|------|
| 修改 | `shared/include/PacketHeader.h` | padding[3] → flags + padding[2], 加 FrameFlags 枚举 |
| 修改 | `host/src/main.cpp` | 精简到 220 行，只留截图+压缩+发送+热键 |
| 修改 | `host/include/UdpSender.h` | SendCompressedFrame 加 uint8_t flags 参数 |
| 修改 | `host/src/UdpSender.cpp` | 实现中写入 header->flags |
| 修改 | `host/CMakeLists.txt` | 删 HttpTuner/MouseController/UdpReplyReceiver, 删 dll/web copy |
| 新增 | `client/include/PdController.h` | 目标选择 + PD 算法 |
| 新增 | `client/src/PdController.cpp` | 从 host/main.cpp + MouseController.cpp 搬来 |
| 新增 | `client/include/MouseDevice.h` | MAKCU 封装 |
| 新增 | `client/src/MouseDevice.cpp` | makcu::Device 连接/移动 |
| 复制 | `client/include/HttpTuner.h` | 从 host 搬来（统计适配副机） |
| 复制 | `client/src/HttpTuner.cpp` | 从 host 搬来 |
| 复制 | `client/web/` | 从 host/web/ 搬来 |
| 修改 | `client/src/main.cpp` | Consumer 加 PdController+MouseDevice, 删 ReplySender |
| 修改 | `client/CMakeLists.txt` | 加 makcu-cpp/httplib/web copy, 删 ReplySender |
| 新增 | `client/include/UdpReceiver.h` 修改 | 加 GetLastFlags() 接口 |
| 新增 | `client/src/UdpReceiver.cpp` 修改 | 解析 PacketHeader.flags |
| 删除 | `host/include/UdpReplyReceiver.h` | |
| 删除 | `host/src/UdpReplyReceiver.cpp` | |
| 删除 | `host/include/MouseController.h` | |
| 删除 | `host/src/MouseController.cpp` | |
| 删除 | `host/include/HttpTuner.h` | |
| 删除 | `host/src/HttpTuner.cpp` | |
| 删除 | `host/web/` | |
| 删除 | `host/mousedll/` | |
| 删除 | `client/include/UdpReplySender.h` | |
| 删除 | `client/src/UdpReplySender.cpp` | |

## 六、运行时变更

### 启动顺序

```
1. 副机先启动:
   SynapseX_Client.exe 8888 ../../model/bf416.engine

2. 主机后启动:
   SynapseX_Host.exe 192.168.100.2 8888 416 416

3. 浏览器打开副机面板:
   http://192.168.100.2:9999
```

### 热键

- PageUp/PageDown 仍然在**主机键盘**上按（玩家在主机上玩游戏）
- 主机检测到热键 → 设置 `PacketHeader.flags` 的 `FF_AIM_ENABLED` 位
- 副机收到帧后读取 flags → 更新 `HttpTuner.m_state.hostAimEnabled`
- Web 面板显示当前热键状态

### 网络拓扑

```
Host:  192.168.100.1  UDP :8888 发送
                      无入站端口

Client: 192.168.100.2 UDP :8888 接收
                        TCP :9999 HttpTuner (web 面板)
                        USB → MAKCU → USB HID → 主机鼠标
```
