#pragma once

#include <cstdint>
#include <cstddef>
#include <linux/input.h>

class VirtualGamepad {
public:
    VirtualGamepad();
    ~VirtualGamepad() { Destroy(); }

    VirtualGamepad(const VirtualGamepad&) = delete;
    VirtualGamepad& operator=(const VirtualGamepad&) = delete;

    bool Create();
    void Destroy();

    bool IsValid() const { return m_fd >= 0; }

    void Update(const uint8_t* buf, size_t n);

private:
    void Emit(uint16_t type, uint16_t code, int32_t value);
    void EmitSyn();

    int m_fd = -1;

    // Last-known state for change detection
    uint8_t m_prevButtons[3]{};
    uint8_t m_prevFlags{};
    int32_t m_prevTriggerL{}, m_prevTriggerR{};
    int32_t m_prevLX{}, m_prevLY{}, m_prevRX{}, m_prevRY{};
    int32_t m_prevHatX{}, m_prevHatY{};

    int32_t m_prevTP1X{}, m_prevTP1Y{}, m_prevTP1C{};
    int32_t m_prevTP2X{}, m_prevTP2Y{}, m_prevTP2C{};
    int32_t m_tp1TrackingId{-1}, m_tp2TrackingId{-1};
};
