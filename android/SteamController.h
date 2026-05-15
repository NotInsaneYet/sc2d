#pragma once

#include "HidDevice.h"

#include <atomic>
#include <cstdint>
#include <thread>

class SteamController {
public:
    static constexpr uint16_t VALVE_VID        = 0x28DE;
    static constexpr uint16_t SC2026_PID       = 0x1302;
    static constexpr uint16_t SC2026_DONGLE_PID = 0x1304;
    static constexpr uint16_t VENDOR_USAGE_PAGE = 0xFF00;

    // Input report IDs
    static constexpr uint8_t REPORT_STATE    = 0x42;
    static constexpr uint8_t REPORT_SECONDARY = 0x43;
    static constexpr uint8_t REPORT_STATUS   = 0x44;
    static constexpr uint8_t REPORT_EXTENDED = 0x45;

    // Feature report command channel
    static constexpr uint8_t FEATURE_REPORT_CMD  = 0x01;
    static constexpr uint8_t FEATURE_REPORT_CMD2 = 0x02;

    // Command bytes
    static constexpr uint8_t CMD_CLEAR_DIGITAL_MAPPINGS = 0x81;
    static constexpr uint8_t CMD_GET_DIGITAL_MAPPINGS   = 0x82;
    static constexpr uint8_t CMD_SET_DEFAULT_MAPPINGS   = 0x85;
    static constexpr uint8_t CMD_SET_SETTINGS           = 0x87;

    // Setting key IDs
    static constexpr uint8_t SETTING_RIGHT_TRACKPAD_MODE = 0x07;
    static constexpr uint8_t SETTING_LEFT_TRACKPAD_MODE  = 0x08;
    static constexpr uint8_t TRACKPAD_NONE               = 0x00;

    // ---- Button bit positions (same as Windows version) ----
    // Prefixed with SC_ to avoid collision with linux/input.h macros (BTN_A etc.)

    // buf[02] — button byte 0
    static constexpr uint8_t SC_BTN_A      = 0x01;
    static constexpr uint8_t SC_BTN_B      = 0x02;
    static constexpr uint8_t SC_BTN_X      = 0x04;
    static constexpr uint8_t SC_BTN_Y      = 0x08;
    static constexpr uint8_t SC_BTN_RS     = 0x20;
    static constexpr uint8_t SC_BTN_MENU   = 0x40;
    static constexpr uint8_t SC_BTN_R4     = 0x80;

    // buf[03] — button byte 1
    static constexpr uint8_t SC_BTN_R5      = 0x01;
    static constexpr uint8_t SC_BTN_RB      = 0x02;
    static constexpr uint8_t SC_DPAD_DN = 0x04;
    static constexpr uint8_t SC_DPAD_RT = 0x08;
    static constexpr uint8_t SC_DPAD_LT = 0x10;
    static constexpr uint8_t SC_DPAD_UP = 0x20;
    static constexpr uint8_t SC_BTN_VIEW    = 0x40;
    static constexpr uint8_t SC_BTN_LS      = 0x80;

    // buf[04] — button byte 2
    static constexpr uint8_t SC_BTN_STEAM    = 0x01;
    static constexpr uint8_t SC_BTN_L4       = 0x02;
    static constexpr uint8_t SC_BTN_L5       = 0x04;
    static constexpr uint8_t SC_BTN_LB       = 0x08;
    static constexpr uint8_t SC_BTN_RS_TOUCH  = 0x10;
    static constexpr uint8_t SC_BTN_TP_RT    = 0x20;
    static constexpr uint8_t SC_BTN_RT_FULL  = 0x80;

    // buf[05] — flags byte
    static constexpr uint8_t SC_BTN_LS_TOUCH    = 0x01;
    static constexpr uint8_t SC_BTN_TP_LT      = 0x02;
    static constexpr uint8_t SC_TP_LT_CLICK = 0x04;
    static constexpr uint8_t SC_FLAG_GRIP_RT = 0x10;
    static constexpr uint8_t SC_FLAG_GRIP_LT = 0x20;

    // ---- Input report byte offsets ----
    // buf[06..07]  left trigger  16-bit LE signed
    // buf[08..09]  right trigger 16-bit LE signed
    // buf[10..11]  left stick X  16-bit LE signed
    // buf[12..13]  left stick Y  16-bit LE signed
    // buf[14..15]  right stick X 16-bit LE signed
    // buf[16..17]  right stick Y 16-bit LE signed
    // buf[18..19]  left trackpad X 16-bit LE signed
    // buf[20..21]  left trackpad Y 16-bit LE signed
    // buf[22..23]  left trackpad contact area
    // buf[24..25]  right trackpad X 16-bit LE signed
    // buf[26..27]  right trackpad Y 16-bit LE signed
    // buf[28..29]  right trackpad contact area
    // buf[30..31]  constant 0x03 0x46
    // buf[32..39]  IMU quaternion 4x 16-bit LE
    // buf[44..45]  battery 0xFFFF = full

    static constexpr int REPORT_SIZE = 64;

    SteamController() = default;
    ~SteamController() { Close(); }

    SteamController(const SteamController&) = delete;
    SteamController& operator=(const SteamController&) = delete;

    bool Open();
    void Close();
    bool IsOpen() const { return m_device.IsOpen(); }

    bool DisableLizardMode();
    bool EnableLizardMode();

    static void ReleaseStuckKeys();

    size_t ReadReport(uint8_t* buffer, size_t size, uint32_t timeoutMs = 16);

    // Drain incoming reports until the specified button mask bytes are all zero
    // (i.e. physical buttons released). TimeoutMs is total time before giving up.
    void DrainUntilReleased(uint8_t maskB2, uint8_t maskB3, uint8_t maskB4,
                            uint32_t timeoutMs = 2000);

    // Parse helpers — extract 16-bit LE value from report
    static int16_t ParseInt16(const uint8_t* buf, int offset)
    {
        return static_cast<int16_t>(
            static_cast<uint16_t>(buf[offset]) |
            (static_cast<uint16_t>(buf[offset + 1]) << 8));
    }

private:
    void HeartbeatLoop();

    HidDevice           m_device;
    std::thread         m_heartbeat;
    std::atomic<bool>   m_running{false};
};
