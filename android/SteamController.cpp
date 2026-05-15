#include "SteamController.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Command buffer builder
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Open / Close
// ---------------------------------------------------------------------------

bool SteamController::Open()
{
    for (uint16_t pid : { SC2026_PID, SC2026_DONGLE_PID }) {
        auto paths = HidDevice::Enumerate(VALVE_VID, pid, /*usagePage=*/0);
        if (paths.empty())
            continue;

        for (auto const& path : paths) {
            if (!m_device.Open(path))
                continue;

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

// ---------------------------------------------------------------------------
// Lizard mode
// ---------------------------------------------------------------------------

bool SteamController::DisableLizardMode()
{
    uint8_t buf[64];

    // Step 1 — kill keyboard/mouse button mappings
    BuildCmd(buf, CMD_CLEAR_DIGITAL_MAPPINGS);
    if (!m_device.SendFeatureReport(buf, sizeof(buf))) {
        fprintf(stderr, "  CLEAR_DIGITAL_MAPPINGS failed\n");
        return false;
    }

    // Step 2 — set trackpads to NONE mode
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

    // Two-step re-enable: first stop the current clear, then restore defaults
    BuildCmd(buf, CMD_CLEAR_DIGITAL_MAPPINGS);
    if (!m_device.SendFeatureReport(buf, sizeof(buf)))
        return false;

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    BuildCmd(buf, CMD_SET_DEFAULT_MAPPINGS);
    bool ok = m_device.SendFeatureReport(buf, sizeof(buf));

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return ok;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

size_t SteamController::ReadReport(uint8_t* buffer, size_t size,
                                    uint32_t timeoutMs)
{
    return m_device.ReadInputReport(buffer, size, timeoutMs);
}

// ---------------------------------------------------------------------------
// DrainUntilReleased — wait for physical buttons to come up
// ---------------------------------------------------------------------------

void SteamController::DrainUntilReleased(uint8_t maskB2, uint8_t maskB3,
                                          uint8_t maskB4, uint32_t timeoutMs)
{
    auto deadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(timeoutMs);
    uint8_t buf[64];

    printf("  draining combo buttons...\n");
    while (std::chrono::steady_clock::now() < deadline) {
        size_t n = ReadReport(buf, sizeof(buf), 50);
        if (n == 0 || buf[0] != REPORT_STATE)
            continue;
        bool stillHeld = (buf[2] & maskB2) || (buf[3] & maskB3) || (buf[4] & maskB4);
        if (!stillHeld) {
            printf("  combo buttons released\n");
            return;
        }
    }
    printf("  drain timeout — forcing lizard switch\n");
}

// ---------------------------------------------------------------------------
// ReleaseStuckKeys — scan sysfs for all Valve input event devices and
// write EV_KEY release for every scancode + SYN_REPORT. This directly
// clears stuck key-down on the actual keyboard device, which a separate
// uinput device cannot reach.
// ---------------------------------------------------------------------------

void SteamController::ReleaseStuckKeys()
{
    DIR* dir = opendir("/sys/class/input");
    if (!dir) return;

    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (strncmp(ent->d_name, "event", 5) != 0)
            continue;

        char path[256];
        snprintf(path, sizeof(path), "/sys/class/input/%s/device/id/vendor",
                 ent->d_name);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        unsigned v = 0;
        fscanf(f, "%x", &v);
        fclose(f);
        if (v != VALVE_VID)
            continue;

        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        int fd = open(path, O_RDWR);
        if (fd < 0)
            continue;

        struct input_event ev{};
        ev.type = EV_KEY;
        ev.value = 0;
        for (int code = 1; code < 256; ++code) {
            ev.code = code;
            write(fd, &ev, sizeof(ev));
        }
        ev.type = EV_SYN;
        ev.code = SYN_REPORT;
        ev.value = 0;
        write(fd, &ev, sizeof(ev));

        close(fd);
        printf("  flushed stuck keys on %s\n", ent->d_name);
    }
    closedir(dir);
}

// ---------------------------------------------------------------------------
// Heartbeat — keeps lizard mode off
// ---------------------------------------------------------------------------

void SteamController::HeartbeatLoop()
{
    uint8_t buf[64];
    BuildCmd(buf, CMD_CLEAR_DIGITAL_MAPPINGS);

    while (m_running.load()) {
        m_device.SendFeatureReport(buf, sizeof(buf));
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
    }
}
