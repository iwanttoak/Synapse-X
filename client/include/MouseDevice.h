#pragma once
#include <cstdint>

namespace makcu {
class Device;
}

namespace SynapseX {

class MouseDevice {
public:
    MouseDevice() = default;
    ~MouseDevice();

    bool Connect();
    void Move(int dx, int dy);
    void Disconnect();
    bool IsConnected() const;

private:
    makcu::Device* m_device = nullptr;
    bool m_connected = false;
};

} // namespace SynapseX
