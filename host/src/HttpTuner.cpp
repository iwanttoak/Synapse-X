// ─── HttpTuner.cpp ────────────────────────────────────────────
// 用于实时瞄准参数调整的内嵌 HTTP 服务器。
// 从局域网中任何设备访问 http://<主机IP>:9999

#include "HttpTuner.h"
#include "Log.h"

#include "httplib.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <chrono>

namespace SynapseX {

// ═══════════════════════════════════════════════════════════════
//  JSON 辅助函数（无第三方库 — 为我们的微型载荷手写）
// ═══════════════════════════════════════════════════════════════

static std::string jsonEscape(double v) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.4f", v);
    return buf;
}
static std::string jsonStr(const char* key, double val) {
    return std::string("\"") + key + "\":" + jsonEscape(val);
}
static std::string jsonStr(const char* key, int val) {
    return std::string("\"") + key + "\":" + std::to_string(val);
}
static std::string jsonStr(const char* key, bool val) {
    return std::string("\"") + key + "\":" + (val ? "true" : "false");
}
static std::string jsonStr(const char* key, uint64_t val) {
    return std::string("\"") + key + "\":" + std::to_string(val);
}

// 简单的浮点数解析器（std::stof 也可用，但这里明确实现）
static float parseFloat(const std::string& s) {
    return static_cast<float>(std::atof(s.c_str()));
}

// 从 JSON 中解析浮点数值，如："smoothFactor":0.15
static bool extractFloat(const std::string& body, const char* key, float& out) {
    std::string search = std::string("\"") + key + "\":";
    auto pos = body.find(search);
    if (pos == std::string::npos) return false;
    pos += search.size();
    // 跳过空白
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) pos++;
    auto end = pos;
    while (end < body.size() && (body[end] == '-' || body[end] == '.' ||
           (body[end] >= '0' && body[end] <= '9'))) end++;
    if (end == pos) return false;
    out = parseFloat(body.substr(pos, end - pos));
    return true;
}
static bool extractBool(const std::string& body, const char* key, bool& out) {
    std::string search = std::string("\"") + key + "\":";
    auto pos = body.find(search);
    if (pos == std::string::npos) return false;
    pos += search.size();
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) pos++;
    if (body.compare(pos, 4, "true") == 0) { out = true; return true; }
    if (body.compare(pos, 5, "false") == 0) { out = false; return true; }
    return false;
}

// ═══════════════════════════════════════════════════════════════
//  生命周期
// ═══════════════════════════════════════════════════════════════

HttpTuner::~HttpTuner() {
    Stop();
}

bool HttpTuner::Start(uint16_t port) {
    if (m_state.running) Stop();

    m_state.serverPort = port;
    m_state.running = true;
    m_thread = std::thread(&HttpTuner::ServerThread, this);

    // 短暂等待服务器绑定
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    SX_LOG_INFO("[HttpTuner] 控制面板线程已在端口 {}", port);
    SX_LOG_INFO("[HttpTuner] 本地访问 http://localhost:{} 或从其他设备访问 http://<主机IP>:{}",
                port, port);
    return true;
}

void HttpTuner::Stop() {
    if (m_state.running) {
        m_state.running = false;
        // httplib 的 stop() 通过类似析构函数的行为
        // 在加入线程时调用。我们通过 running=false 发出信号。
        if (m_thread.joinable()) {
            m_thread.join();
        }
        SX_LOG_INFO("[HttpTuner] 控制面板已停止");
    }
}

// ═══════════════════════════════════════════════════════════════
//  服务器线程
// ═══════════════════════════════════════════════════════════════

void HttpTuner::ServerThread() {
    httplib::Server svr;

    // ── GET / — 从磁盘提供控制面板页面 ────────────
    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        std::ifstream f("web/index.html");
        if (f) {
            std::string html((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
            res.set_content(html, "text/html; charset=utf-8");
        } else {
            SX_LOG_WARN("[HttpTuner] web/index.html 未找到；提供回退文本");
            res.set_content("找不到 web/index.html。"
                            "请将其放在 exe 旁边或工作目录中。",
                            "text/plain");
        }
    });

    // ── GET /api/state — 返回 JSON 快照 ──────────
    // 通过 httplib 的用户数据机制捕获 'this' 指针...
    // 实际上，我们直接使用 lambda 捕获。
    // httplib 处理程序是同步的，因此我们可以安全地在
    // 处理程序内部锁定互斥量。
    //
    // 问题：处理程序中需要 'this'。使用原始指针。
    svr.Get("/api/state", [this](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(m_mutex);

        std::string json = "{";
        // 配置
        json += "\"config\":{";
        json += jsonStr("Kp",            m_state.config.Kp) + ",";
        json += jsonStr("Kd",            m_state.config.Kd) + ",";
        json += jsonStr("aimRange",      (int)m_state.config.aimRange) + ",";
        json += jsonStr("minConfidence",       m_state.config.minConfidence) + ",";
        json += jsonStr("deltaHeadConfidence", m_state.config.deltaHeadConfidence) + ",";
        json += jsonStr("aimPoint",            m_state.config.aimPoint) + ",";
        json += jsonStr("headOffset",    m_state.config.headOffset) + ",";
        json += jsonStr("kpMax",        m_state.config.kpMax) + ",";
        json += jsonStr("kpDecay",      m_state.config.kpDecay) + ",";
        json += jsonStr("gameW",        (int)m_state.config.gameW) + ",";
        json += jsonStr("gameH",        (int)m_state.config.gameH) + ",";
        json += jsonStr("nativeW",      (int)m_state.config.nativeW) + ",";
        json += jsonStr("nativeH",      (int)m_state.config.nativeH) + ",";
        json += jsonStr("modelId",      (int)m_state.config.modelId) + ",";
        json += jsonStr("classFilter",  m_state.config.classFilter) + ",";
        json += jsonStr("aimEnabled",    m_state.aimEnabled);
        json += "},";
        // 统计
        json += jsonStr("sendFps",    m_state.sendFps) + ",";
        json += jsonStr("captureFps", m_state.captureFps) + ",";
        json += jsonStr("pipelineMs", m_state.pipelineMs) + ",";
        json += jsonStr("compressMs", m_state.compressMs) + ",";
        json += jsonStr("freshFrames", m_state.freshFrames) + ",";
        json += jsonStr("cacheFrames", m_state.cacheFrames) + ",";
        json += jsonStr("totalSent",  m_state.totalSent) + ",";
        // 目标
        json += "\"target\":{";
        json += jsonStr("active",     m_state.target.active) + ",";
        json += jsonStr("screenX",    m_state.target.screenX) + ",";
        json += jsonStr("screenY",    m_state.target.screenY) + ",";
        json += jsonStr("confidence", m_state.target.confidence) + ",";
        json += jsonStr("distance",   m_state.target.distance) + ",";
        json += jsonStr("classId",    m_state.target.classId);
        json += "}";
        json += "}";

        res.set_content(json, "application/json");
    });

    // ── POST /api/config — 更新参数 ───────────
    svr.Post("/api/config", [this](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto& body = req.body;

        float f;
        bool  b;
        if (extractFloat(body, "Kp", f))            m_state.config.Kp            = f;
        if (extractFloat(body, "Kd", f))            m_state.config.Kd            = f;
        if (extractFloat(body, "aimRange", f))      m_state.config.aimRange      = f;
        if (extractFloat(body, "minConfidence", f))       m_state.config.minConfidence       = f;
        if (extractFloat(body, "deltaHeadConfidence", f)) m_state.config.deltaHeadConfidence = f;
        if (extractFloat(body, "headOffset", f))          m_state.config.headOffset          = f;
        if (extractFloat(body, "kpMax", f))              m_state.config.kpMax              = f;
        if (extractFloat(body, "kpDecay", f))            m_state.config.kpDecay            = f;
        if (extractFloat(body, "gameW", f))         m_state.config.gameW         = (int)f;
        if (extractFloat(body, "gameH", f))         m_state.config.gameH         = (int)f;
        if (extractFloat(body, "modelId", f))       m_state.config.modelId       = (uint8_t)f;
        if (extractFloat(body, "classFilter", f))   m_state.config.classFilter   = (int)f;
        if (extractFloat(body, "aimPoint", f))      m_state.config.aimPoint      = (int)f;
        if (extractBool(body, "aimEnabled", b))     m_state.aimEnabled           = b;

        SX_LOG_DEBUG("[HttpTuner] 配置已更新: Kp={:.3f}, Kd={:.3f}, aimRange={:.1f}, minConfidence={:.2f}, modelId={}, classFilter={}, aimEnabled={}",
                     m_state.config.Kp,
                     m_state.config.Kd,
                     m_state.config.aimRange,
                     m_state.config.minConfidence,
                     static_cast<int>(m_state.config.modelId),
                     m_state.config.classFilter,
                     m_state.aimEnabled);
        res.set_content("{\"ok\":true}", "application/json");
    });

    // ── 绑定并服务 ─────────────────────────────────
    svr.set_keep_alive_max_count(1);
    SX_LOG_INFO("[HttpTuner] HTTP 服务器正在绑定到 0.0.0.0:{}", m_state.serverPort);
    if (!svr.listen("0.0.0.0", m_state.serverPort)) {
        SX_LOG_ERROR("[HttpTuner] HTTP 服务器已停止或无法绑定到端口 {}",
                     m_state.serverPort);
    }
}

// ═══════════════════════════════════════════════════════════════
//  线程安全的访问器
// ═══════════════════════════════════════════════════════════════

AimConfig HttpTuner::GetConfig() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state.config;
}

bool HttpTuner::IsAimEnabled() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state.aimEnabled;
}

void HttpTuner::SetAimEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_state.aimEnabled = enabled;
}

void HttpTuner::UpdateStats(double sendFps, double captureFps,
                             double pipelineMs, double compressMs,
                             int fresh, int cache, uint64_t totalSent) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_state.sendFps     = sendFps;
    m_state.captureFps  = captureFps;
    m_state.pipelineMs  = pipelineMs;
    m_state.compressMs  = compressMs;
    m_state.freshFrames = fresh;
    m_state.cacheFrames = cache;
    m_state.totalSent   = totalSent;
}

void HttpTuner::UpdateTarget(float screenX, float screenY,
                              float confidence, float distance, int classId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_state.target.active     = true;
    m_state.target.screenX    = screenX;
    m_state.target.screenY    = screenY;
    m_state.target.confidence = confidence;
    m_state.target.distance   = distance;
    m_state.target.classId    = classId;
}

} // namespace SynapseX
