// ─── BMP 采集测试 ────────────────────────────────────────────
// 验证 DxgiCapturer 的 ROI 截屏功能。
// 采集一帧 → 写入 32-bit BMP → 退出。
//
// 编译：
//   cd host
//   cmake --build build_x64 --config RelWithDebInfo --target SynapseX_Host_TestBmp
//   .\build_x64\RelWithDebInfo\SynapseX_Host_TestBmp.exe

#include "DxgiCapturer.h"
#include "Log.h"
#include "Lz4Compressor.h"

#include <windows.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// ═══════════════════════════════════════════════════════════════
//  手写 BMP 写入器（零第三方依赖）
//  输入：BGRA 像素 (DXGI_FORMAT_B8G8R8A8_UNORM 原生格式)
//  输出：32 位 BMP，兼容所有主流图片查看器
// ═══════════════════════════════════════════════════════════════

bool SaveBgraAsBmp(const char* path,
                   const uint8_t* pixels,
                   int width,
                   int height)
{
    int rowSize   = width * 4;                       // BGRA = 每像素 4 字节
    int padSize   = (4 - (rowSize % 4)) % 4;
    int rowStride = rowSize + padSize;
    int imageSize = rowStride * height;

    // BITMAPFILEHEADER（14 字节）
    BITMAPFILEHEADER bf = {};
    bf.bfType      = 0x4D42;                         // 'BM'
    bf.bfSize      = sizeof(BITMAPFILEHEADER)
                   + sizeof(BITMAPINFOHEADER)
                   + imageSize;
    bf.bfReserved1 = 0;
    bf.bfReserved2 = 0;
    bf.bfOffBits   = sizeof(BITMAPFILEHEADER)
                   + sizeof(BITMAPINFOHEADER);

    // BITMAPINFOHEADER（40 字节）
    BITMAPINFOHEADER bi = {};
    bi.biSize          = sizeof(BITMAPINFOHEADER);
    bi.biWidth         = width;
    bi.biHeight        = -height;                   // 负值 = 自上而下的 DIB（无需翻转）
    bi.biPlanes        = 1;
    bi.biBitCount      = 32;
    bi.biCompression   = BI_RGB;
    bi.biSizeImage     = imageSize;
    bi.biXPelsPerMeter = 0;
    bi.biYPelsPerMeter = 0;
    bi.biClrUsed       = 0;
    bi.biClrImportant  = 0;

    FILE* f = fopen(path, "wb");
    if (!f) {
        SX_LOG_ERROR("[HostTest] 无法打开 BMP 输出文件: {}", path);
        return false;
    }

    fwrite(&bf, sizeof(bf), 1, f);
    fwrite(&bi, sizeof(bi), 1, f);

    const uint8_t* row = pixels;
    uint8_t padding[4] = {0, 0, 0, 0};

    for (int y = 0; y < height; ++y) {
        fwrite(row, 1, rowSize, f);
        if (padSize > 0) {
            fwrite(padding, 1, padSize, f);
        }
        row += rowSize;
    }

    fclose(f);
    return true;
}

} // 匿名命名空间

// ═══════════════════════════════════════════════════════════════
//  测试入口
// ═══════════════════════════════════════════════════════════════

int main() {
    SynapseX::Log::Initialize("host_test", spdlog::level::debug, false);
    SX_LOG_INFO("[HostTest] DXGI ROI 采集测试已启动");

    // ── 1. 初始化 ────────────────────────────────────
    SynapseX::DxgiCapturer capturer;
    constexpr int ROI_W = 640;
    constexpr int ROI_H = 640;

    if (!capturer.Initialize(ROI_W, ROI_H)) {
        SX_LOG_CRITICAL("[HostTest] DxgiCapturer 初始化失败。可能原因：未连接显示器、驱动不支持或桌面复制不可用");
        return 1;
    }

    SX_LOG_INFO("[HostTest] 显示器分辨率={}x{} ROI={}x{}",
                capturer.GetOutputWidth(), capturer.GetOutputHeight(),
                ROI_W, ROI_H);

    // 计算预期的源区域用于诊断
    LONG srcLeft = (capturer.GetOutputWidth()  - ROI_W) / 2;
    LONG srcTop  = (capturer.GetOutputHeight() - ROI_H) / 2;
    SX_LOG_INFO("[HostTest] 源矩形: 左={} 上={} 右={} 下={}",
                srcLeft, srcTop, srcLeft + ROI_W, srcTop + ROI_H);

    SX_LOG_INFO("[HostTest] 等待新帧；移动窗口或鼠标以触发桌面更新");

    // ── 2. 采集循环（等待真实帧）──────────────────────
    std::vector<uint8_t> buffer;
    constexpr int kMaxAttempts = 300;
    bool captured = false;

    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        if (capturer.CaptureFrame(buffer)) {
            // 检查帧数据是否非零
            bool allZero = true;
            for (size_t i = 0; i < buffer.size() && allZero; ++i) {
                if (buffer[i] != 0) allZero = false;
            }

            const auto& info = capturer.GetLastFrameInfo();
            SX_LOG_DEBUG("[HostTest] 尝试={} 字节={} 上次呈现时间={} 累计帧数={} 元数据字节={} 指针=({}, {}) 可见={} 受保护={} 全零={}",
                         attempt,
                         buffer.size(),
                         static_cast<long long>(info.LastPresentTime.QuadPart),
                         info.AccumulatedFrames,
                         info.TotalMetadataBufferSize,
                         static_cast<int>(info.PointerPosition.Position.x),
                         static_cast<int>(info.PointerPosition.Position.y),
                         info.PointerPosition.Visible,
                         info.ProtectedContentMaskedOut,
                         allZero);

            if (!allZero) {
                captured = true;
                SX_LOG_INFO("[HostTest] 在第 {} 次尝试中采集到非零帧", attempt);
                break;
            }
        }
        Sleep(10);
    }

    if (!captured) {
        SX_LOG_ERROR("[HostTest] 全部 {} 次采集尝试均返回黑帧。请检查远程桌面、虚拟显示器、物理显示器/GPU连接以及 ProtectedContentMaskedOut 诊断信息",
                     kMaxAttempts);
        return 1;
    }

    // ── 3. 保存 BMP ────────────────────────────────────
    const char* outPath = "test_roi.bmp";
    if (!SaveBgraAsBmp(outPath, buffer.data(), ROI_W, ROI_H)) {
        SX_LOG_CRITICAL("[HostTest] 无法写入 BMP 输出");
        return 1;
    }

    SX_LOG_INFO("[HostTest] 已保存 BMP: 路径={} 尺寸={}x{} 字节={}",
                outPath, ROI_W, ROI_H, buffer.size());

    // 显示前几个像素供人工验证
    if (buffer.size() >= 16) {
        const uint8_t* p = buffer.data();
        SX_LOG_DEBUG("[HostTest] 前4个像素 BGRA: [{},{},{},{}] [{},{},{},{}] [{},{},{},{}] [{},{},{},{}]",
                     p[0],p[1],p[2],p[3], p[4],p[5],p[6],p[7],
                     p[8],p[9],p[10],p[11], p[12],p[13],p[14],p[15]);
    }

    // ── 4. LZ4 压缩测试 ────────────────────────────────
    SX_LOG_INFO("[HostTest] 开始 LZ4 压缩测试");

    const int rawSize = static_cast<int>(buffer.size());

    // 初始化压缩器（预分配内部缓冲区）
    SynapseX::Lz4Compressor compressor;
    if (!compressor.Initialize(rawSize)) {
        SX_LOG_CRITICAL("[HostTest] LZ4 压缩器初始化失败");
        return 1;
    }
    SX_LOG_INFO("[HostTest] LZ4 压缩器就绪: 最大输入={} 最大输出={}",
                compressor.GetMaxInputSize(),
                SynapseX::Lz4Compressor::GetMaxOutputSize(rawSize));

    // 压缩并计时
    std::vector<uint8_t> compressed;
    auto t0 = std::chrono::high_resolution_clock::now();

    if (!compressor.Compress(buffer.data(), rawSize, compressed)) {
        SX_LOG_CRITICAL("[HostTest] LZ4 压缩失败");
        return 1;
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double compressMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // 统计
    int compressedSize = static_cast<int>(compressed.size());
    double ratio = (rawSize > 0)
        ? (100.0 * compressedSize / rawSize)
        : 0.0;
    double throughputMBps = (compressMs > 0.0)
        ? (rawSize / (1024.0 * 1024.0)) / (compressMs / 1000.0)
        : 0.0;

    SX_LOG_INFO("[HostTest] LZ4 结果: 原始字节={} 压缩字节={} 压缩率={:.1f}% 压缩耗时={:.3f} 吞吐量MB/s={:.1f}",
                rawSize, compressedSize, ratio, compressMs, throughputMBps);
    SX_LOG_INFO("[HostTest] 所有测试通过");
    return 0;
}
