#pragma once

// ─── TrtInference ──────────────────────────────────────────────
// TensorRT 推理封装，支持热切换引擎。
//
// 用法：
//   TrtInference trt;
//   trt.Initialize();                        // 创建CUDA运行时
//   trt.LoadEngine(0);                       // 加载模型ID 0
//   // 在热循环中：
//   auto dets = trt.Infer(bgraPixels, 0.25f); // 自动切换模型
//                                              // 如果 g_targetModelId 已更改
//
// 线程安全：Infer() 仅在 Consumer 线程中调用。
// g_targetModelId 由 Producer 线程（UdpReceiver）写入。

#include <cstdint>
#include <string>
#include <vector>

namespace SynapseX {

struct Detection {
    float x1, y1;
    float x2, y2;
    float confidence;
    int   classId;
};

class TrtInference {
public:
    TrtInference() = default;
    ~TrtInference();

    TrtInference(const TrtInference&) = delete;
    TrtInference& operator=(const TrtInference&) = delete;
    TrtInference(TrtInference&&) = delete;
    TrtInference& operator=(TrtInference&&) = delete;

    // 创建CUDA运行时。尚未加载引擎。
    // 调用一次，然后调用 LoadEngine() 或让 Infer() 自动切换。
    bool Initialize();

    // 按ID加载指定模型。如有旧引擎则销毁。
    //  0: apex_enemy_416     1: delta_body_head_416
    //  2: bf6_enemy_self_416  3: ow2_enemy_416
    bool LoadEngine(uint8_t modelId);

    // 直接通过文件路径加载引擎（用于独立测试）。
    // 如有旧引擎则销毁。modelId 仅用于跟踪。
    bool LoadEngineByPath(const std::string& path, uint8_t modelId = 254);

    // 创建专用CUDA流。在 Consumer 线程中调用一次。
    bool SetupStream();

    // 运行推理。如果自上次调用以来 g_targetModelId 已更改，
    // 则同步流，销毁旧引擎，加载新模型，
    // 并返回空结果（调用方应传入下一帧）。
    std::vector<Detection> Infer(const uint8_t* bgra,
                                 float confThr = 0.25f);

    void Cleanup();
    bool IsInitialized() const { return m_initialized; }
    bool HasEngine()      const { return m_context != nullptr; }

    int GetModelWidth()   const { return m_modelW; }
    int GetModelHeight()  const { return m_modelH; }
    uint8_t GetCurrentModelId() const { return m_currentModelId; }

private:
    // 映射 modelId → 引擎文件路径
    static std::string GetModelPath(uint8_t modelId);

    // 销毁TRT引擎 + 上下文 + GPU缓冲区（保留运行时和流）
    void UnloadEngine();

    // 从文件加载引擎，反序列化，创建上下文，分配IO
    bool LoadEngineFile(const std::string& path);

    bool m_initialized = false;

    int m_modelW = 416;
    int m_modelH = 416;
    int m_numDets = 300;

    uint8_t m_currentModelId = 255;  // 无效 → 强制首次加载

    // TRT 对象
    void* m_runtime   = nullptr;  // nvinfer1::IRuntime*（跨重载持久存在）
    void* m_engine    = nullptr;  // nvinfer1::ICudaEngine*
    void* m_context   = nullptr;  // nvinfer1::IExecutionContext*

    // GPU 缓冲区
    void*  m_dInput     = nullptr;
    void*  m_dBgraInput = nullptr;
    void*  m_dOutput    = nullptr;
    size_t m_outputBytes = 0;
    void*  m_stream     = nullptr;  // cudaStream_t（跨重载持久存在）
};

} // namespace SynapseX
