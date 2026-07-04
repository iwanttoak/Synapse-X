// ─── BMP 采集测试 ────────────────────────────────────────────
// 验证 DxgiCapturer 的 ROI 截屏功能。
// 采集一帧 → 写入 32-bit BMP → 退出。
//
// 编译：
//   cd host
//   cmake --build build_x64 --config RelWithDebInfo --target SynapseX_Host_TestBmp
//   .\build_x64\RelWithDebInfo\SynapseX_Host_TestBmp.exe

#include "DxgiCapturer.h"
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
        fprintf(stderr, "[BMP] 无法打开文件: %s\n", path);
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
    fprintf(stderr, "============================================\n");
    fprintf(stderr, "  Synapse-X: DXGI ROI 截屏测试\n");
    fprintf(stderr, "============================================\n\n");

    // ── 1. 初始化 ────────────────────────────────────
    SynapseX::DxgiCapturer capturer;
    constexpr int ROI_W = 640;
    constexpr int ROI_H = 640;

    if (!capturer.Initialize(ROI_W, ROI_H)) {
        fprintf(stderr, "[致命错误] DXGI 截屏器初始化失败。\n");
        fprintf(stderr, "  可能原因：无显示器、显卡驱动不支持、\n");
        fprintf(stderr, "  或此输出设备不支持桌面复制。\n");
        return 1;
    }

    fprintf(stderr, "[信息] 显示器分辨率: %dx%d\n",
            capturer.GetOutputWidth(), capturer.GetOutputHeight());
    fprintf(stderr, "[信息] ROI: %dx%d（中心裁切）\n", ROI_W, ROI_H);

    // 计算预期的源区域用于诊断
    LONG srcLeft = (capturer.GetOutputWidth()  - ROI_W) / 2;
    LONG srcTop  = (capturer.GetOutputHeight() - ROI_H) / 2;
    fprintf(stderr, "[信息] 源区域: left=%ld top=%ld right=%ld bottom=%ld\n",
            srcLeft, srcTop, srcLeft + ROI_W, srcTop + ROI_H);

    fprintf(stderr, "[信息] 等待新帧...\n");
    fprintf(stderr, "       （移动窗口或晃动鼠标以触发画面更新）\n\n");

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
            fprintf(stderr, "[尝试 %3d] 获取到帧: %zu 字节 | "
                    "LastPresentTime=%lld AccumulatedFrames=%u RectCount=%u "
                    "PtrPos=(%d,%d) Visible=%d Protected=%u AllZero=%s\n",
                    attempt,
                    buffer.size(),
                    static_cast<long long>(info.LastPresentTime.QuadPart),
                    info.AccumulatedFrames,
                    info.TotalMetadataBufferSize,
                    static_cast<int>(info.PointerPosition.Position.x),
                    static_cast<int>(info.PointerPosition.Position.y),
                    info.PointerPosition.Visible,
                    info.ProtectedContentMaskedOut,
                    allZero ? "是" : "否");

            if (!allZero) {
                captured = true;
                fprintf(stderr, "[成功] 第 %d 次尝试捕获到非零帧！\n", attempt);
                break;
            }
        }
        Sleep(10);
    }

    if (!captured) {
        fprintf(stderr, "\n[错误] 全部 %d 帧均为黑色（全零像素）。\n",
                kMaxAttempts);
        fprintf(stderr, "排查建议：\n");
        fprintf(stderr, "  1. 是否在远程桌面/虚拟显示器上运行？\n");
        fprintf(stderr, "  2. 显示器是否已开机并连接到此 GPU？\n");
        fprintf(stderr, "  3. 尝试在物理桌面上以控制台应用运行。\n");
        fprintf(stderr, "  4. 检查上述日志中 ProtectedContentMaskedOut 是否被设置。\n");
        return 1;
    }

    // ── 3. 保存 BMP ────────────────────────────────────
    const char* outPath = "test_roi.bmp";
    if (!SaveBgraAsBmp(outPath, buffer.data(), ROI_W, ROI_H)) {
        fprintf(stderr, "[致命错误] BMP 写入失败。\n");
        return 1;
    }

    fprintf(stderr, "[成功] BMP 已保存: %s (%dx%d, 32-bit BGRA, %zu 字节)\n",
            outPath, ROI_W, ROI_H, buffer.size());

    // 显示前几个像素供人工验证
    if (buffer.size() >= 16) {
        const uint8_t* p = buffer.data();
        fprintf(stderr, "[信息] 前 4 个像素 (B,G,R,A): "
                "[%3u,%3u,%3u,%3u] [%3u,%3u,%3u,%3u] "
                "[%3u,%3u,%3u,%3u] [%3u,%3u,%3u,%3u]\n",
                p[0],p[1],p[2],p[3],   p[4],p[5],p[6],p[7],
                p[8],p[9],p[10],p[11], p[12],p[13],p[14],p[15]);
    }

    // ── 4. LZ4 压缩测试 ────────────────────────────────
    fprintf(stderr, "\n============================================\n");
    fprintf(stderr, "  LZ4 压缩测试\n");
    fprintf(stderr, "============================================\n");

    const int rawSize = static_cast<int>(buffer.size());

    // 初始化压缩器（预分配内部缓冲区）
    SynapseX::Lz4Compressor compressor;
    if (!compressor.Initialize(rawSize)) {
        fprintf(stderr, "[致命错误] LZ4 压缩器初始化失败。\n");
        return 1;
    }
    fprintf(stderr, "[信息] 压缩器已初始化，最大输入: %d 字节\n",
            compressor.GetMaxInputSize());
    fprintf(stderr, "[信息] 最坏情况压缩缓冲区: %d 字节 (LZ4_compressBound)\n\n",
            SynapseX::Lz4Compressor::GetMaxOutputSize(rawSize));

    // 压缩并计时
    std::vector<uint8_t> compressed;
    auto t0 = std::chrono::high_resolution_clock::now();

    if (!compressor.Compress(buffer.data(), rawSize, compressed)) {
        fprintf(stderr, "[致命错误] LZ4 压缩失败。\n");
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

    fprintf(stderr, "[LZ4] 压缩结果:\n");
    fprintf(stderr, "  原始大小:     %10d 字节  (%.2f MB)\n",
            rawSize, rawSize / (1024.0 * 1024.0));
    fprintf(stderr, "  压缩后:       %10d 字节  (%.2f MB)\n",
            compressedSize, compressedSize / (1024.0 * 1024.0));
    fprintf(stderr, "  压缩比:       %10.1f %%  (占原始)\n", ratio);
    fprintf(stderr, "  耗时:         %10.3f 毫秒\n", compressMs);
    fprintf(stderr, "  吞吐量:       %10.1f MB/s\n", throughputMBps);

    fprintf(stderr, "\n[完成] 所有测试通过。\n");
    return 0;
}
