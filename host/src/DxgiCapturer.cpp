// ─── DxgiCapturer.cpp ──────────────────────────────────────────
// DXGI 桌面复制采集模块
//
// 数据流（尽可能在 GPU 端进行）：
//   桌面帧（显存）
//        |
//        v CopySubresourceRegion（仅 ROI）
//   暂存纹理（显存，CPU_ACCESS_READ）
//        |
//        v Map / 逐行 memcpy / Unmap
//   std::vector<uint8_t>（系统内存，BGRA 连续排列）

#include "DxgiCapturer.h"
#include "Log.h"

#include <cstring>
#include <cstdio>
#include <thread>

namespace SynapseX {

static std::string Narrow(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') {
        return {};
    }

    int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }

    std::string result(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    return result;
}

// ═══════════════════════════════════════════════════════════════
//  构造函数 / 析构函数
// ═══════════════════════════════════════════════════════════════

DxgiCapturer::~DxgiCapturer() {
    Cleanup();
}

// ═══════════════════════════════════════════════════════════════
//  公开 API
// ═══════════════════════════════════════════════════════════════

bool DxgiCapturer::Initialize(int roiWidth, int roiHeight) {
    if (m_initialized) {
        Cleanup();
    }

    m_roiWidth  = roiWidth;
    m_roiHeight = roiHeight;

    if (!CreateDeviceAndDuplication()) {
        SX_LOG_ERROR("[DxgiCapturer] CreateDeviceAndDuplication() returned false during initialization");
        Cleanup();
        return false;
    }

    m_initialized = true;
    SX_LOG_INFO("[DxgiCapturer] Ready: output={}x{}, roi={}x{}, crop=center",
                m_outputWidth, m_outputHeight, m_roiWidth, m_roiHeight);
    return true;
}

bool DxgiCapturer::CaptureFrame(std::vector<uint8_t>& outBuffer) {
    // 冷启动重建：如果之前失败，冷却后重试
    if (!m_initialized || !m_duplication) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_lastRebuildAttempt).count();
        if (elapsed >= kRebuildCooldownMs) {
            m_lastRebuildAttempt = now;
            SX_LOG_WARN("[DxgiCapturer] Capture path unavailable, attempting rebuild after {} ms cooldown",
                        static_cast<int>(elapsed));
            if (CreateDeviceAndDuplication()) {
                SX_LOG_INFO("[DxgiCapturer] Rebuild succeeded after cold start");
                m_initialized = true;
                // 继续执行下面的采集尝试
            }
        }
        if (!m_initialized) return false;
    }

    if (m_recreating) {
        return false;
    }

    // ── 1. 获取桌面帧 ──────────────────────────
    DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
    Microsoft::WRL::ComPtr<IDXGIResource> desktopResource;

    HRESULT hr = m_duplication->AcquireNextFrame(
        kAcquireTimeoutMs,
        &frameInfo,
        desktopResource.GetAddressOf()
    );

    // 无新帧 -- 正常情况，返回 false
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return false;
    }

    // 全屏独占丢失 / 模式切换 -- 自动重建
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_lastRebuildAttempt).count();

        if (elapsed < kRebuildCooldownMs) {
            return false;  // 冷却中 -- 暂时不再重试
        }
        m_lastRebuildAttempt = now;

        SX_LOG_WARN("[DxgiCapturer] DXGI access lost, rebuilding duplication after {} ms",
                    static_cast<int>(elapsed));
        m_recreating = true;
        ReleaseResources();

        // 短暂休眠 -- 转换期间游戏可能仍持有独占输出
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (CreateDeviceAndDuplication()) {
            SX_LOG_INFO("[DxgiCapturer] Rebuild succeeded after DXGI access loss");
            m_recreating = false;
            m_initialized = true;
        } else {
            SX_LOG_ERROR("[DxgiCapturer] Rebuild failed after DXGI access loss; will retry after cooldown");
            m_initialized = false;
            m_recreating = false;
        }
        return false;
    }

    if (FAILED(hr)) {
        SX_LOG_ERROR("[DxgiCapturer] AcquireNextFrame failed: HRESULT=0x{:08X}",
                     static_cast<unsigned>(hr));
        return false;
    }

    m_lastFrameInfo = frameInfo;  // 缓存用于诊断

    // ── 2. 查询桌面纹理 ──────────────────────────
    Microsoft::WRL::ComPtr<ID3D11Texture2D> desktopTexture;
    hr = desktopResource.As(&desktopTexture);
    if (FAILED(hr)) {
        SX_LOG_ERROR("[DxgiCapturer] QI -> ID3D11Texture2D failed: HRESULT=0x{:08X}",
                     static_cast<unsigned>(hr));
        m_duplication->ReleaseFrame();
        return false;
    }

    // ── 3. GPU 复制：桌面 ROI -> 暂存纹理 ───────
    LONG srcLeft = (static_cast<LONG>(m_outputWidth)  - static_cast<LONG>(m_roiWidth))  / 2;
    LONG srcTop  = (static_cast<LONG>(m_outputHeight) - static_cast<LONG>(m_roiHeight)) / 2;
    if (srcLeft < 0) srcLeft = 0;
    if (srcTop  < 0) srcTop  = 0;

    D3D11_BOX srcBox = {};
    srcBox.left   = static_cast<UINT>(srcLeft);
    srcBox.top    = static_cast<UINT>(srcTop);
    srcBox.right  = srcBox.left + static_cast<UINT>(m_roiWidth);
    srcBox.bottom = srcBox.top  + static_cast<UINT>(m_roiHeight);
    srcBox.front  = 0;
    srcBox.back   = 1;

    if (srcBox.right  > static_cast<UINT>(m_outputWidth))  srcBox.right  = m_outputWidth;
    if (srcBox.bottom > static_cast<UINT>(m_outputHeight)) srcBox.bottom = m_outputHeight;

    m_context->CopySubresourceRegion(
        m_stagingTexture.Get(),   // 目标
        0, 0, 0, 0,              // 目标子资源, 目标 X,Y,Z
        desktopTexture.Get(),     // 源
        0,                        // 源子资源
        &srcBox                   // 源区域（仅 ROI）
    );

    // 刷新 GPU 管线，确保 CopySubresourceRegion 完成
    // 之后再调用 ReleaseFrame（后者可能使桌面纹理无效）。
    m_context->Flush();

    // ── 4. 释放桌面帧 ──────────────────────────
    m_duplication->ReleaseFrame();

    // ── 5. 映射暂存纹理 -> 逐行复制 ─────────
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = m_context->Map(m_stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        SX_LOG_ERROR("[DxgiCapturer] Map(staging_texture) failed: HRESULT=0x{:08X}",
                     static_cast<unsigned>(hr));
        return false;
    }

    UINT rowSize = m_roiWidth * kBytesPerPixel;
    outBuffer.resize(static_cast<size_t>(m_roiHeight) * rowSize);

    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
    uint8_t*       dst = outBuffer.data();

    for (int row = 0; row < m_roiHeight; ++row) {
        std::memcpy(
            dst + static_cast<size_t>(row) * rowSize,
            src + static_cast<size_t>(row) * mapped.RowPitch,
            rowSize
        );
    }

    m_context->Unmap(m_stagingTexture.Get(), 0);

    return true;
}

void DxgiCapturer::Cleanup() {
    ReleaseResources();
    m_initialized   = false;
    m_outputWidth   = 0;
    m_outputHeight  = 0;
}

// ═══════════════════════════════════════════════════════════════
//  私有：设备 / 复制接口创建
// ═══════════════════════════════════════════════════════════════

bool DxgiCapturer::CreateDeviceAndDuplication() {
    // ── 步骤 1：创建 D3D11 设备 ──────────────────────
    const D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT hr = D3D11CreateDevice(
        nullptr,                              // pAdapter（默认 GPU）
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createFlags,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        m_device.GetAddressOf(),
        nullptr,                              // pFeatureLevel
        m_context.GetAddressOf()
    );

    if (FAILED(hr) && (createFlags & D3D11_CREATE_DEVICE_DEBUG)) {
        SX_LOG_WARN("[DxgiCapturer] D3D11 debug layer unavailable, falling back to release device");
        createFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            createFlags, featureLevels, ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION,
            m_device.GetAddressOf(), nullptr, m_context.GetAddressOf()
        );
    }

    if (FAILED(hr)) {
        SX_LOG_ERROR("[DxgiCapturer] D3D11CreateDevice failed: HRESULT=0x{:08X}",
                     static_cast<unsigned>(hr));
        return false;
    }

    // ── 步骤 2：D3D11Device -> IDXGIDevice -> IDXGIAdapter
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    hr = m_device.As(&dxgiDevice);
    if (FAILED(hr)) {
        SX_LOG_ERROR("[DxgiCapturer] QI -> IDXGIDevice failed: HRESULT=0x{:08X}",
                     static_cast<unsigned>(hr));
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(adapter.GetAddressOf());
    if (FAILED(hr)) {
        SX_LOG_ERROR("[DxgiCapturer] IDXGIDevice::GetAdapter failed: HRESULT=0x{:08X}",
                     static_cast<unsigned>(hr));
        return false;
    }

    // 记录适配器描述信息，用于多 GPU 调试
    DXGI_ADAPTER_DESC adapterDesc = {};
    adapter->GetDesc(&adapterDesc);
    SX_LOG_DEBUG("[DxgiCapturer] Adapter='{}' vendor=0x{:04X} device=0x{:04X}",
                 Narrow(adapterDesc.Description),
                 adapterDesc.VendorId,
                 adapterDesc.DeviceId);

    // ── 步骤 3：适配器 -> 枚举输出 -> IDXGIOutput1
    // 枚举所有输出来找到活动输出
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    int outputIndex = 0;
    bool foundOutput = false;

    for (; outputIndex < 16; ++outputIndex) {
        hr = adapter->EnumOutputs(outputIndex, output.GetAddressOf());
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(hr)) continue;

        DXGI_OUTPUT_DESC desc = {};
        hr = output->GetDesc(&desc);
        if (FAILED(hr)) continue;

        // 检查该输出是否有非零桌面区域
        int w = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
        int h = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;

        SX_LOG_DEBUG("[DxgiCapturer] Output[{}]='{}' size={}x{} origin=({}, {}) attached={}",
                     outputIndex,
                     Narrow(desc.DeviceName),
                     w,
                     h,
                     static_cast<long>(desc.DesktopCoordinates.left),
                     static_cast<long>(desc.DesktopCoordinates.top),
                     desc.AttachedToDesktop);

        if (desc.AttachedToDesktop && w > 0 && h > 0) {
            m_outputWidth  = w;
            m_outputHeight = h;
            foundOutput = true;
            break;  // 使用第一个已连接的桌面输出
        }
    }

    if (!foundOutput) {
        SX_LOG_ERROR("[DxgiCapturer] No attached desktop output found after scanning {} outputs",
                     outputIndex);
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
    hr = output.As(&output1);
    if (FAILED(hr)) {
        SX_LOG_ERROR("[DxgiCapturer] QI -> IDXGIOutput1 failed: HRESULT=0x{:08X}",
                     static_cast<unsigned>(hr));
        return false;
    }

    // ── 步骤 4：创建桌面复制接口 ─────
    hr = output1->DuplicateOutput(m_device.Get(), m_duplication.GetAddressOf());
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        SX_LOG_ERROR("[DxgiCapturer] DuplicateOutput unsupported by current driver or display");
        return false;
    }
    if (FAILED(hr)) {
        SX_LOG_ERROR("[DxgiCapturer] DuplicateOutput failed: HRESULT=0x{:08X}",
                     static_cast<unsigned>(hr));
        return false;
    }

    // ── 步骤 5：创建暂存纹理（ROI 大小，CPU 可读）
    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width              = static_cast<UINT>(m_roiWidth);
    stagingDesc.Height             = static_cast<UINT>(m_roiHeight);
    stagingDesc.MipLevels          = 1;
    stagingDesc.ArraySize          = 1;
    stagingDesc.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
    stagingDesc.SampleDesc.Count   = 1;
    stagingDesc.SampleDesc.Quality = 0;
    stagingDesc.Usage              = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags          = 0;
    stagingDesc.CPUAccessFlags     = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags          = 0;

    hr = m_device->CreateTexture2D(&stagingDesc, nullptr, m_stagingTexture.GetAddressOf());
    if (FAILED(hr)) {
        SX_LOG_ERROR("[DxgiCapturer] CreateTexture2D(staging {}x{}) failed: HRESULT=0x{:08X}",
                     m_roiWidth, m_roiHeight, static_cast<unsigned>(hr));
        return false;
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════
//  私有：资源释放
// ═══════════════════════════════════════════════════════════════

void DxgiCapturer::ReleaseResources() {
    m_stagingTexture.Reset();
    m_duplication.Reset();
    m_context.Reset();
    m_device.Reset();
}

} // namespace SynapseX
