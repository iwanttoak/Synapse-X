# 鼠标控制规范

> 主机端辅助瞄准流水线的完整规范。
> 涵盖：目标选择、瞄准点计算、PD 控制器、死区、
> DLL 集成、网页调优参数及所有可配置常量。

---

## 1. 架构概览

```
main.cpp（170 Hz 节拍）
  │
  ├── 从 UdpReplyReceiver 接收回复
  │     └── Detection[]（屏幕空间, classId, confidence）
  │
  ├── 目标选择
  │     ├── 过滤：仅敌人（classId == 0）
  │     ├── 排序：最高置信度，平局按距屏幕中心最近
  │     └── 输出：最佳 Detection*
  │
  ├── 瞄准点计算
  │     ├── aimPoint=0（身体）：目标 = 边界框中心 → (x1+x2)/2, (y1+y2)/2
  │     └── aimPoint=1（头部）：目标 = 边界框顶部 → (x1+x2)/2, y1 + bboxH * headOffset
  │
  ├── 误差向量
  │     ├── dx = targetCx - screenW/2
  │     ├── dy = targetCy - screenH/2
  │     └── 将 (dx, dy) 传递给 MouseController::AimAtTarget()
  │
  └── MouseController（PD 控制器）
        ├── 死区检查
        ├── 范围门控
        ├── PD 计算：Output = Kp*error + Kd*dE
        ├── 量化 + 最小步长强制
        └── ddll64.dll MoveR(moveX, moveY)
```

### 坐标空间

| 空间 | 原点 | 单位 | 产生者 |
|-------|--------|------|-----------------|
| 模型像素 | ROI 左上角 | px（0–415） | 客户端（TRT 推理） |
| 屏幕 | 全屏显示左上角 | px（0–3839） | `UdpReplyReceiver` 通过 `screen = roiOffset + model` 将模型映射到屏幕 |
| 瞄准误差 | 屏幕中心 | px 偏移 | `main.cpp`：`dx = target - screenCenter` |

---

## 2. 目标选择（main.cpp）

### 算法

```
输入：  std::vector<Detection> — 当前客户端回复中的所有检测
输出： const Detection* best  — 若无有效目标则为 nullptr

1. 对于每个检测 d：
     if d.classId != 0: 跳过        // 仅敌人
     cx = (d.x1 + d.x2) / 2         // 边界框中心
     cy = (d.y1 + d.y2) / 2
     dist = sqrt((cx-scrCx)² + (cy-scrCy)²)

2. 选择最佳：
     if 尚无最佳: 选择此检测
     elif d.confidence > best.confidence: 选择此检测
     elif d.confidence == best.confidence && dist < bestDist: 选择此检测
```

### 备注

- **单目标锁定**：每帧仅一个目标。无多目标平均。
- **无时间粘性**：如果置信度变化，立即切换目标。
  （已知问题 A3 — 未来修复：切换前需要 N 帧确认。）
- **忽略队友（classId=1）**：仅敌人目标触发瞄准。

---

## 3. 瞄准点计算（main.cpp）

确定瞄准目标边界框的何处。

```
float bboxH = best->y2 - best->y1;

targetCx = (best->x1 + best->x2) * 0.5f;  // 始终水平居中

if (aimPoint == 1):  // 头部
    targetCy = best->y1 + bboxH * headOffset;
else:                // 身体（默认）
    targetCy = (best->y1 + best->y2) * 0.5f;
```

### 参数

| 参数 | 类型 | 默认值 | 范围 | 描述 |
|-----------|------|---------|-------|-------------|
| `aimPoint` | int | 0 | 0 或 1 | 0=身体中心，1=头部 |
| `headOffset` | float | 0.12 | 0.05–0.25 | 距边界框顶部的比例（头部位置） |

### 示意图

```
┌─────────────┐ ← y1
│   headOff=12%│ ← aimPoint=1：此处瞄准（顶部 12%）
│             │
│     × center│ ← aimPoint=0：此处瞄准（几何中心）
│             │
└─────────────┘ ← y2 = y1 + bboxH
```

---

## 4. 误差向量（main.cpp）

传递给 PD 控制器的屏幕中心偏移量：

```
float dx = targetCx - screenW * 0.5f;
float dy = targetCy - screenH * 0.5f;
```

- `dx > 0` → 目标在准星右侧
- `dy > 0` → 目标在准星下方
- 两者均为**物理屏幕像素**（非归一化，非模型空间）

---

## 5. PD 控制器（MouseController::AimAtTarget）

### 5.1 流程图

```
AimAtTarget(dx, dy, confidence, cfg)   ← dx,dy = 视觉误差
  │
  ├─ 守卫：!m_loaded → 返回 false
  ├─ 守卫：confidence < cfg.minConfidence → 重置所有状态，返回 false
  │
  ├─ 步骤 1：延迟补偿
  │    sumSentX/Y = 环形缓冲区中最近 2 帧 MoveR 值的总和
  │    realDx = dx - sumSentX
  │    realDy = dy - sumSentY
  │
  ├─ 步骤 2：微死区（基于补偿后的误差）
  │    pixelError = sqrt(realDx² + realDy²)
  │    if pixelError < 1.5 px → 重置所有状态，返回 false
  │
  ├─ 步骤 3：范围门控
  │    if pixelError > cfg.aimRange → 重置所有状态，返回 false
  │
  ├─ 步骤 4：动态 Kp（指数衰减）
  │    currentKp = Kp_base + (kpMax − Kp_base) × e^(−kpDecay × pixelError)
  │    钳位到 [Kp, kpMax]
  │
  ├─ 步骤 5：D 项（基于补偿后的误差变化）
  │    dError = realError − prevError（仅当 hasPrevError 时）
  │    D = Kd × dError
  │
  ├─ 步骤 6：PD 输出
  │    Output = currentKp × realError + Kd × dError
  │
  ├─ 步骤 7：亚像素累加器
  │    residualX += OutputX
  │    residualY += OutputY
  │    if |residualX| ≥ 1.0: moveX = int(residualX); residualX -= moveX
  │    同理 Y
  │
  ├─ 步骤 8：执行
  │    if moveX != 0 || moveY != 0: MoveR(moveX, moveY)
  │
  ├─ 步骤 9：记录到延迟环形缓冲区
  │    sentHistory[writeIdx] = {moveX, moveY}
  │    writeIdx = (writeIdx + 1) % 2
  │
  └─ 步骤 10：保存 PD 状态
       prevErrorX = realDx, prevErrorY = realDy
       hasPrevError = true
```

### 5.2 动态 Kp 公式

```
pixelError = √(realDx² + realDy²)

Kp_actual = Kp_base + (kpMax − Kp_base) × e^(−kpDecay × pixelError)

                                           ←──── ──── →  ← 远距离用 base Kp，贴脸 boost 到 kpMax

示例（Kp=0.30, kpMax=0.75, kpDecay=0.05）：
  200px → Kp≈0.30 （平缓跟踪）
   50px → Kp≈0.38 （接近中）
   10px → Kp≈0.57 （磁吸）
    0px → Kp=0.75 （最大锁定）

每轴 PD 输出：
  Output = currentKp × realError + Kd × (realError − prevError)

  residual += Output                                   ← 亚像素累加器
  if |residual| ≥ 1.0: 发出 MoveR(int(residual))，减去整数部分
```

**为什么不做 dt 除法**：在固定 170Hz 下，`dt ≈ 0.00588s` 实际上为常数。
除以 dt 会放大导数噪声 170 倍。直接使用 `dE`（每帧像素数）
使 Kd 调优更直观。

**为什么没有 I 项**：积分会累积过去的误差，导致目标切换时
积分饱和以及在近距离过冲。在 170Hz 下，仅 P+D 就足够了。

**为什么没有 dE 死区**：老的 `|dE| < 0.5` 门控阻碍了慢速跟踪
运动。亚像素累加器现在自然地处理了这个问题——亚像素
输出逐帧累积，直到发出完整的像素移动，
没有硬性截止干扰。

### 5.3 延迟补偿

在 FPS 游戏中，`MoveR(dx,dy)` 旋转的是摄像机角度，而非光标位置。
视觉流水线有 1–2 帧的延迟（捕获→网络→推理→回复→瞄准）。
PD 控制器会看到"旧的"误差，该误差尚未反映最近发送的移动，
导致其重新施加推力并过冲。

**解决方案**：在 PD 计算之前，从视觉误差中减去最近 2 帧已发送
`MoveR` 值的总和：

```
realDx = dx − sum(sentMoveX[0..kDelayFrames-1])
realDy = dy − sum(sentMoveY[0..kDelayFrames-1])
```

`realDx/realDy` 随后被送入 PD 控制器。这告诉控制器：
"这是扣除了已在途移动后的误差。"

环形缓冲区（`m_sentHistory[2]`）存储最近 2 帧中实际发出给
`MoveR` 的 (moveX, moveY) 值。当状态重置时（目标丢失、死区、
重新获取），缓冲区被归零——不同方向上的旧移动
不得污染新目标的误差。

### 5.4 亚像素累加器

替代了老的强制 ±1 量化和 dE 死区。

```
m_residualX += outputX;
m_residualY += outputY;

int moveX = 0, moveY = 0;

if (m_residualX ≥ 1.0f) {
    moveX = (int)m_residualX;        // 例如 1.7 → 1
    m_residualX -= (float)moveX;     // 保留 0.7 用于下一帧
} else if (m_residualX ≤ −1.0f) {
    moveX = (int)m_residualX;
    m_residualX -= (float)moveX;
}
// 同理 Y 轴
```

这就像一个水桶——每帧有小部分输出流入。
当水桶装满到 ≥1 像素时，发出一次 `MoveR`，并且该像素被
"花掉"。剩余部分结转到下一帧。

**为什么这消除了抖动**：
- 在死区边缘不再有强制 ±1——如果输出为 0.3，直到它自然
  累积超过 1.0 之前什么也不会发生
- 亚像素跟踪非常平滑——准星可以每 2–3 帧移动 1px，
  没有任何量化冲击
- 不再需要 dE 死区——合法的慢速跟踪变化
  通过剩余值自然累积

### 5.5 噪声门控

| 层级 | 类型 | 阈值 | 效果 |
|-------|------|-----------|--------|
| 微死区 | 空间 | `pixelError < 1.5px` | 在目标附近停止所有移动。 |
| 范围门控 | 空间 | `pixelError > aimRange` | 在视野半径外不触发瞄准。 |
| 置信度门控 | 检测 | `conf < minConfidence` | 丢弃低置信度检测。 |

老的 3px 死区、dt 跟踪和导数死区已被移除。
动态 Kp 在远距离自然衰减至基础 Kp（不需要额外阻尼）。

### 5.5 状态重置触发条件

| 触发条件 | 效果 |
|---------|--------|
| `confidence < minConfidence` | 重置（prevError=0, 无 lastTime） |
| `dist < deadzone (3px)` | 重置——目标已获取，无需移动 |
| `dist > aimRange` | 重置——超出范围 |
| `dt > 50ms`（帧间隔） | 强制 prevError = 当前误差（防止 D 尖峰） |
| 加载后首次调用 | 强制 prevError = 当前误差 |

重置将清除 `prevErrorX/Y` 和 `m_hasLastTime`，因此下次调用将
误差视为初始状态——无导数尖峰。

---

## 6. 可配置参数（网页面板）

```cpp
struct AimConfig {
    float Kp              = 0.40f;   // 比例增益
    float Kd              = 0.05f;   // 微分增益（速度阻尼）
    float aimRange        = 500.0f;  // 距中心最大像素距离以触发瞄准
    float minConfidence   = 0.25f;   // 低于此值忽略
    int   aimPoint        = 0;       // 0=身体, 1=头部
    float headOffset      = 0.12f;   // 头部瞄准：边界框顶部比例
};
```

| 参数 | 网页滑块范围 | 步长 | 作用 |
|-----------|-----------------|------|------|
| `Kp` | 0.05 – 1.50 | 0.01 | **基础 Kp**（远距离跟踪）。越高 = 快速甩枪。 |
| `Kd` | 0.00 – 0.50 | 0.01 | 制动力。越高 = 越少过冲。0 = 纯 P。 |
| `kpMax` | 0.10 – 1.50 | 0.05 | 贴脸时的最大 Kp。磁吸锁定上限。 |
| `kpDecay` | 0.00 – 0.15 | 0.01 | 衰减陡度。越高 = 仅在极近距离才磁吸。 |
| `aimRange` | 50 – 1000 | 10 | 触发瞄准的半径（像素）。 |
| `minConfidence` | 0.00 – 1.00 | 0.01 | 触发瞄准的最低检测置信度。 |
| `aimPoint` | 0 / 1 | — | 身体中心或头部。 |
| `headOffset` | 0.05 – 0.25 | 0.01 | 头部位置，距边界框顶部的比例。 |

### 调优指南

```
步骤 1：Kd=0, kpMax=Kp。调优 Kp（基础值）以获得平滑的远距离跟踪。
步骤 2：提高 kpMax 直到近距离磁吸感觉有吸力但不抖动。
步骤 3：调优 kpDecay — 越低 = 磁吸在更远处激活，越高 = 仅在贴脸时激活。
步骤 4：以 0.01 为增量添加 Kd 以抑制近距离过冲。
步骤 5：aimRange + minConfidence 与之前相同。

典型配置：
  平滑跟踪：    Kp=0.25  kpMax=0.50  kpDecay=0.03  Kd=0.05
  均衡（默认）：Kp=0.30  kpMax=0.75  kpDecay=0.05  Kd=0.05
  激进磁吸：    Kp=0.40  kpMax=1.20  kpDecay=0.08  Kd=0.10
```

### 已移除的参数

| 参数 | 移除原因 |
|-----------|-------------|
| `smoothFactor` | 被 PD 控制器（Kp + Kd）替代。指数衰减慢、不准确且无阻尼。 |
| `sensitivity` | 冗余 — `Output = (P+D) × sensitivity` 意味着 sensitivity 和 Kp 相互冲突。改为直接调优 Kp。 |
| `smoothFactor`（指数衰减） | 被 PD 控制器（Kp + Kd）替代。 |
| `predictionFrames`, `emaAlpha`, `predictThreshold` | 速度前馈在视觉延迟下不稳定。被动态 Kp（基于距离的增强）替代。 |
| `EMA filter` | PD 亚像素累加器使其不再必要。 |

---

## 7. DLL 集成（ddll64.dll）

### 加载

```cpp
MouseController mouse;
mouse.Load("ddll64.dll");
// DLL 通过 CMake 构建后步骤复制到 exe 目录
// 来源：host/mousedll/ddll64.dll
```

### 使用的 API

| 函数 | 签名 | 调用频率 |
|----------|-----------|----------------|
| `OpenDevice` | `int OpenDevice()` | 初始化时一次 |
| `MoveR` | `void MoveR(int dx, int dy)` | 最高 170 Hz |
| `FreeLibrary` |（析构函数） | 关闭时一次 |

### 要求

- **管理员权限**：`OpenDevice` 无提升权限时返回 0。
- **64 位**：`ddll64.dll` 是 64 位 DLL；必须与 64 位主机端可执行文件匹配。
- **同目录**：DLL 必须与 exe 在同一目录或在 PATH 中。

---

## 8. 线程与安全性

- **单线程**：所有瞄准逻辑在主循环线程上以 170Hz 运行。
  `AimAtTarget` 内部无互斥锁——配置通过 `GetConfig()` 每节拍读取一次
  （这是从 `HttpTuner` 的互斥保护状态复制而来）。
- **PD 状态（`m_prevErrorX/Y`, `m_lastTime`）**：非线程安全。仅
  从主循环线程访问。
- **网页配置更新**：`HttpTuner` 在后台线程运行。配置
  通过 `GetConfig()`（互斥保护的复制）每节拍读取一次。该副本随后
  按值传递给 `AimAtTarget`。

---

## 9. 性能

| 指标 | 值 |
|--------|-------|
| 调用频率 | 170 Hz（每个节拍，即使无检测） |
| 堆分配 | **0** — 所有数学运算在栈上，无向量 |
| 浮点精度 | `float`（32 位）— 足以满足像素精度 |
| `sqrt` 调用 | 每节拍 1 次（距离检查） |
| 分支数 | 约 6 个分支（守卫、死区、范围、置信度、量化） |
| 间接调用 | 1 个函数指针调用（`m_moveR`）到 DLL |
| 增加延迟 | < 1 µs（纯数学运算 + 一次 DLL 调用） |

---

## 10. 文件索引

| 文件 | 作用 |
|------|------|
| `host/include/MouseController.h` | `AimConfig` 结构体, `MouseController` 类 |
| `host/src/MouseController.cpp` | PD 控制器实现, DLL 加载 |
| `host/src/main.cpp`（约 L248–310） | 目标选择, 瞄准点, 误差向量, 调用 `AimAtTarget` |
| `host/include/HttpTuner.h` | `TuningState`（为网页面板持有 `AimConfig`） |
| `host/src/HttpTuner.cpp` | 网页 UI：Kp/Kd 滑块, `/api/config` 解析, `/api/state` 序列化 |
| `host/mousedll/ddll64.dll` | 鼠标输入 DLL（提交到仓库） |
