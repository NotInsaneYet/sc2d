#include "SteamController.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

static void BuildCmd(uint8_t (&buf)[64], uint8_t cmd,
                     const uint8_t* payload = nullptr,
                     uint8_t payloadSize = 0)
{
    std::memset(buf, 0, 64);
    buf[0] = SteamController::FEATURE_REPORT_CMD;
    buf[1] = cmd;
    buf[2] = payloadSize;
    if (payload && payloadSize)
        std::memcpy(buf + 3, payload, payloadSize);
}

bool SteamController::Open()
{
    for (uint16_t pid : { SC2026_PID, SC2026_DONGLE_PID }) {
        auto paths = HidDevice::Enumerate(VALVE_VID, pid, /*usagePage=*/0);
        if (paths.empty()) continue;

        for (auto const& path : paths) {
            if (!m_device.Open(path)) continue;

            uint8_t buf[64];
            size_t n = m_device.ReadInputReport(buf, sizeof(buf), 500);
            if (n > 0 && buf[0] == REPORT_STATE) {
                printf("  active interface on PID=%04X\n", pid);
                return true;
            }
            m_device.Close();
        }
    }

    fprintf(stderr, "  no Steam Controller found (wired PID=%04X / dongle PID=%04X)\n",
            SC2026_PID, SC2026_DONGLE_PID);
    return false;
}

void SteamController::Close()
{
    if (m_running.exchange(false) && m_heartbeat.joinable())
        m_heartbeat.join();
    m_device.Close();
}

bool SteamController::DisableLizardMode()
{
    uint8_t buf[64];

    BuildCmd(buf, CMD_CLEAR_DIGITAL_MAPPINGS);
    if (!m_device.SendFeatureReport(buf, sizeof(buf))) {
        fprintf(stderr, "  CLEAR_DIGITAL_MAPPINGS failed\n");
        return false;
    }

    const uint8_t settingsPayload[] = {
        SETTING_LEFT_TRACKPAD_MODE,  0x00, 0x00,
        SETTING_RIGHT_TRACKPAD_MODE, 0x00, 0x00,
    };
    BuildCmd(buf, CMD_SET_SETTINGS, settingsPayload, sizeof(settingsPayload));
    if (!m_device.SendFeatureReport(buf, sizeof(buf))) {
        fprintf(stderr, "  SET_SETTINGS_TRACKPAD failed\n");
        return false;
    }

    if (!m_running.exchange(true))
        m_heartbeat = std::thread(&SteamController::HeartbeatLoop, this);

    return true;
}

bool SteamController::EnableLizardMode()
{
    if (m_running.exchange(false) && m_heartbeat.joinable())
        m_heartbeat.join();

    uint8_t buf[64];

    BuildCmd(buf, CMD_CLEAR_DIGITAL_MAPPINGS);
    m_device.SendFeatureReport(buf, sizeof(buf));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    BuildCmd(buf, CMD_SET_DEFAULT_MAPPINGS);
    bool ok = m_device.SendFeatureReport(buf, sizeof(buf));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return ok;
}

size_t SteamController::ReadReport(uint8_t* buffer, size_t size,
                                    uint32_t timeoutMs)
{
    return m_device.ReadInputReport(buffer, size, timeoutMs);
}

void SteamController::HeartbeatLoop()
{
    uint8_t buf[64];
    BuildCmd(buf, CMD_CLEAR_DIGITAL_MAPPINGS);

    while (m_running.load()) {
        m_device.SendFeatureReport(buf, sizeof(buf));
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
    }
}

void SteamController::SendHaptic(uint16_t strong, uint16_t weak)
{
    // Format confirmed from Steam's own USB traffic via usbmon:
    // [0x80] [type=1] [intensity_LE] [left_speed_LE] [gain=-5dB] [right_speed_LE] [gain=-5dB]
    uint8_t buf[] = {
        0x80,                       // report ID (ID_OUT_REPORT_HAPTIC_RUMBLE)
        1,                          // type
        0x40, 0x1f,                 // intensity (8000, matching Steam's value)
        static_cast<uint8_t>(strong & 0xFF),
        static_cast<uint8_t>(strong >> 8),
        0xfb,                       // left gain (-5 dB)
        static_cast<uint8_t>(weak & 0xFF),
        static_cast<uint8_t>(weak >> 8),
        0xfb,                       // right gain (-5 dB)
    };
    m_device.SendOutputReport(buf, sizeof(buf));
}
