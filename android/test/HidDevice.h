#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

class HidDevice {
public:
    static std::vector<std::string> Enumerate(uint16_t vid, uint16_t pid, uint16_t usagePage = 0);

    HidDevice() = default;
    ~HidDevice() { Close(); }

    HidDevice(const HidDevice&) = delete;
    HidDevice& operator=(const HidDevice&) = delete;

    HidDevice(HidDevice&& o) noexcept
        : m_fd(o.m_fd)
    {
        o.m_fd = -1;
    }

    HidDevice& operator=(HidDevice&& o) noexcept
    {
        if (this != &o) {
            Close();
            m_fd = o.m_fd;
            o.m_fd = -1;
        }
        return *this;
    }

    bool Open(const std::string& path);
    void Close();
    bool IsOpen() const { return m_fd >= 0; }

    bool SendFeatureReport(const uint8_t* data, size_t size);
    bool SendOutputReport(const uint8_t* data, size_t size);
    size_t ReadInputReport(uint8_t* buffer, size_t size, uint32_t timeoutMs = 1000);

private:
    int m_fd = -1;
};
