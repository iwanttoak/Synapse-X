#include "HttpTuner.h"
#include "Log.h"

#include "httplib.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <chrono>

namespace SynapseX {

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

static float parseFloat(const std::string& s) {
    return static_cast<float>(std::atof(s.c_str()));
}

static bool extractFloat(const std::string& body, const char* key, float& out) {
    std::string search = std::string("\"") + key + "\":";
    auto pos = body.find(search);
    if (pos == std::string::npos) return false;
    pos += search.size();
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

HttpTuner::~HttpTuner() { Stop(); }

bool HttpTuner::Start(uint16_t port) {
    if (m_state.running) Stop();

    m_state.serverPort = port;
    m_state.running = true;
    m_thread = std::thread(&HttpTuner::ServerThread, this);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    SX_LOG_INFO("[HttpTuner] 控制面板线程已在端口 {}", port);
    SX_LOG_INFO("[HttpTuner] 访问 http://<副机IP>:{}", port);
    return true;
}

void HttpTuner::Stop() {
    if (m_state.running) {
        m_state.running = false;
        if (m_thread.joinable()) m_thread.join();
        SX_LOG_INFO("[HttpTuner] 控制面板已停止");
    }
}

void HttpTuner::ServerThread() {
    httplib::Server svr;

    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        std::ifstream f("web/index.html");
        if (f) {
            std::string html((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
            res.set_content(html, "text/html; charset=utf-8");
        } else {
            SX_LOG_WARN("[HttpTuner] web/index.html 未找到");
            res.set_content("找不到 web/index.html", "text/plain");
        }
    });

    svr.Get("/api/state", [this](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(m_mutex);

        std::string json = "{";
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
        json += jsonStr("receiveFps",   m_state.receiveFps) + ",";
        json += jsonStr("inferFps",     m_state.inferFps) + ",";
        json += jsonStr("inferMs",      m_state.inferMs) + ",";
        json += jsonStr("pipelineMs",   m_state.pipelineMs) + ",";
        json += jsonStr("freshFrames",  m_state.freshFrames) + ",";
        json += jsonStr("droppedFrames", m_state.droppedFrames) + ",";
        json += "\"target\":{";
        json += jsonStr("active",     m_state.target.active) + ",";
        json += jsonStr("screenX",    m_state.target.screenX) + ",";
        json += jsonStr("screenY",    m_state.target.screenY) + ",";
        json += jsonStr("confidence", m_state.target.confidence) + ",";
        json += jsonStr("distance",   m_state.target.distance) + ",";
        json += jsonStr("dx",         m_state.target.dx) + ",";
        json += jsonStr("dy",         m_state.target.dy);
        json += "},";
        json += jsonStr("hostAimEnabled", m_state.hostAimEnabled);
        json += "}";

        res.set_content(json, "application/json");
    });

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

        SX_LOG_DEBUG("[HttpTuner] 配置已更新: Kp={:.3f}, Kd={:.3f}, aimRange={:.1f}, modelId={}",
                     m_state.config.Kp, m_state.config.Kd, m_state.config.aimRange,
                     static_cast<int>(m_state.config.modelId));
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.set_keep_alive_max_count(1);
    SX_LOG_INFO("[HttpTuner] HTTP 服务器正在绑定到 0.0.0.0:{}", m_state.serverPort);
    if (!svr.listen("0.0.0.0", m_state.serverPort)) {
        SX_LOG_ERROR("[HttpTuner] HTTP 服务器已停止或无法绑定到端口 {}",
                     m_state.serverPort);
    }
}

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

void HttpTuner::UpdateStats(double receiveFps, double inferFps,
                             double inferMs, double pipelineMs,
                             int fresh, int dropped) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_state.receiveFps   = receiveFps;
    m_state.inferFps     = inferFps;
    m_state.inferMs      = inferMs;
    m_state.pipelineMs   = pipelineMs;
    m_state.freshFrames  = fresh;
    m_state.droppedFrames = dropped;
}

void HttpTuner::UpdateTarget(float screenX, float screenY,
                              float confidence, float distance,
                              int dx, int dy) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_state.target.active     = true;
    m_state.target.screenX    = screenX;
    m_state.target.screenY    = screenY;
    m_state.target.confidence = confidence;
    m_state.target.distance   = distance;
    m_state.target.dx         = dx;
    m_state.target.dy         = dy;
}

void HttpTuner::SetHostAimEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_state.hostAimEnabled = enabled;
}

} // namespace SynapseX
