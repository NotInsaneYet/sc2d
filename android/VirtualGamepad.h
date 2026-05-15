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
    void ReleaseAll();

    static void ReleaseAllKeyboardKeys();

    bool IsValid() const { return m_fd >= 0; }

    // Open the RP5's built-in controller as a secondary input source
    // and forward its evdev events to the same uinput device.
    void OpenSecondary();
    void CloseSecondary();
    bool HasSecondary() const { return m_secondaryFd >= 0; }

    // Read pending evdev events from secondary and forward to uinput
    void PollSecondary();

    void Update(const uint8_t* buf, size_t n);

private:
    void Emit(uint16_t type, uint16_t code, int32_t value);
    void EmitSyn();
    static bool NameMatches(const char* name);

    int m_fd = -1;
    int m_secondaryFd = -1;

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
