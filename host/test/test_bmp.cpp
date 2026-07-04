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
        SX_LOG_ERROR("[HostTest] Failed to open BMP output file: {}", path);
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
    SX_LOG_INFO("[HostTest] DXGI ROI capture test started");

    // ── 1. 初始化 ────────────────────────────────────
    SynapseX::DxgiCapturer capturer;
    constexpr int ROI_W = 640;
    constexpr int ROI_H = 640;

    if (!capturer.Initialize(ROI_W, ROI_H)) {
        SX_LOG_CRITICAL("[HostTest] DxgiCapturer initialization failed. Possible causes: no display attached, unsupported driver, or desktop duplication unavailable");
        return 1;
    }

    SX_LOG_INFO("[HostTest] Display resolution={}x{} ROI={}x{}",
                capturer.GetOutputWidth(), capturer.GetOutputHeight(),
                ROI_W, ROI_H);

    // 计算预期的源区域用于诊断
    LONG srcLeft = (capturer.GetOutputWidth()  - ROI_W) / 2;
    LONG srcTop  = (capturer.GetOutputHeight() - ROI_H) / 2;
    SX_LOG_INFO("[HostTest] Source rect: left={} top={} right={} bottom={}",
                srcLeft, srcTop, srcLeft + ROI_W, srcTop + ROI_H);

    SX_LOG_INFO("[HostTest] Waiting for a new frame; move a window or the mouse to trigger desktop updates");

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
            SX_LOG_DEBUG("[HostTest] Attempt={} bytes={} last_present_time={} accumulated_frames={} metadata_bytes={} pointer=({}, {}) visible={} protected={} all_zero={}",
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
                SX_LOG_INFO("[HostTest] Captured a non-zero frame on attempt {}", attempt);
                break;
            }
        }
        Sleep(10);
    }

    if (!captured) {
        SX_LOG_ERROR("[HostTest] All {} capture attempts returned black frames. Check remote desktop, virtual displays, physical monitor/GPU connection, and ProtectedContentMaskedOut diagnostics",
                     kMaxAttempts);
        return 1;
    }

    // ── 3. 保存 BMP ────────────────────────────────────
    const char* outPath = "test_roi.bmp";
    if (!SaveBgraAsBmp(outPath, buffer.data(), ROI_W, ROI_H)) {
        SX_LOG_CRITICAL("[HostTest] Failed to write BMP output");
        return 1;
    }

    SX_LOG_INFO("[HostTest] Saved BMP: path={} size={}x{} bytes={}",
                outPath, ROI_W, ROI_H, buffer.size());

    // 显示前几个像素供人工验证
    if (buffer.size() >= 16) {
        const uint8_t* p = buffer.data();
        SX_LOG_DEBUG("[HostTest] First 4 pixels BGRA: [{},{},{},{}] [{},{},{},{}] [{},{},{},{}] [{},{},{},{}]",
                     p[0],p[1],p[2],p[3], p[4],p[5],p[6],p[7],
                     p[8],p[9],p[10],p[11], p[12],p[13],p[14],p[15]);
    }

    // ── 4. LZ4 压缩测试 ────────────────────────────────
    SX_LOG_INFO("[HostTest] Starting LZ4 compression test");

    const int rawSize = static_cast<int>(buffer.size());

    // 初始化压缩器（预分配内部缓冲区）
    SynapseX::Lz4Compressor compressor;
    if (!compressor.Initialize(rawSize)) {
        SX_LOG_CRITICAL("[HostTest] LZ4 compressor initialization failed");
        return 1;
    }
    SX_LOG_INFO("[HostTest] LZ4 compressor ready: max_input={} max_output={}",
                compressor.GetMaxInputSize(),
                SynapseX::Lz4Compressor::GetMaxOutputSize(rawSize));

    // 压缩并计时
    std::vector<uint8_t> compressed;
    auto t0 = std::chrono::high_resolution_clock::now();

    if (!compressor.Compress(buffer.data(), rawSize, compressed)) {
        SX_LOG_CRITICAL("[HostTest] LZ4 compression failed");
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

    SX_LOG_INFO("[HostTest] LZ4 result: raw_bytes={} compressed_bytes={} ratio={:.1f}% compress_ms={:.3f} throughput_mb_s={:.1f}",
                rawSize, compressedSize, ratio, compressMs, throughputMBps);
    SX_LOG_INFO("[HostTest] All tests passed");
    return 0;
}
