#include "MouseDevice.h"
#include "Log.h"

#ifdef SX_HAS_MAKCU
#include <makcu.h>
#endif

namespace SynapseX {

MouseDevice::~MouseDevice() {
    Disconnect();
}

bool MouseDevice::Connect() {
    if (m_connected) return true;

#ifdef SX_HAS_MAKCU
    std::string port = makcu::Device::findFirstDevice();
    if (port.empty()) {
        SX_LOG_ERROR("[MouseDevice] 未找到 MAKCU 设备");
        return false;
    }

    m_device = new makcu::Device();
    if (!m_device->connect(port, true)) {
        SX_LOG_ERROR("[MouseDevice] MAKCU 设备连接失败");
        delete m_device;
        m_device = nullptr;
        return false;
    }

    m_connected = true;
    SX_LOG_INFO("[MouseDevice] MAKCU 已连接 (端口={})", port);
    return true;
#else
    SX_LOG_WARN("[MouseDevice] MAKCU 未启用；鼠标控制不可用");
    return false;
#endif
}

void MouseDevice::Move(int dx, int dy) {
    if (!m_connected || !m_device) return;
#ifdef SX_HAS_MAKCU
    m_device->mouseMove(dx, dy);
#endif
}

void MouseDevice::Disconnect() {
#ifdef SX_HAS_MAKCU
    if (m_device) {
        m_device->disconnect();
        delete m_device;
        m_device = nullptr;
    }
#endif
    m_connected = false;
}

bool MouseDevice::IsConnected() const {
    return m_connected;
}

} // namespace SynapseX
