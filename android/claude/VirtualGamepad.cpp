#include "VirtualGamepad.h"
#include "SteamController.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <thread>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void VirtualGamepad::Emit(uint16_t type, uint16_t code, int32_t value)
{
    struct input_event ev{};
    ev.type  = type;
    ev.code  = code;
    ev.value = value;
    (void)write(m_fd, &ev, sizeof(ev));
}

void VirtualGamepad::EmitSyn()
{
    Emit(EV_SYN, SYN_REPORT, 0);
}

static int16_t Read16LE(const uint8_t* buf, int off)
{
    return static_cast<int16_t>(
        static_cast<uint16_t>(buf[off]) |
        (static_cast<uint16_t>(buf[off + 1]) << 8));
}

// Map s16 (-32768..32767) to u16 (0..65535) for MT protocol
static int32_t RemapToU16(int16_t v)
{
    return static_cast<int32_t>(static_cast<uint16_t>(v + 32768));
}

// ---------------------------------------------------------------------------
// Create missing /dev/input/eventX node (Android uinput quirk)
// ---------------------------------------------------------------------------

static void FixEventNode()
{
    char newestInput[32] = "";
    char newestEvent[32] = "";
    int newestNum = -1;
    int major = 0, minor = 0;

    DIR* dir = opendir("/sys/class/input");
    if (!dir) return;

    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (strncmp(ent->d_name, "input", 5) != 0) continue;

        char namePath[256];
        snprintf(namePath, sizeof(namePath),
                 "/sys/class/input/%s/name", ent->d_name);
        FILE* f = fopen(namePath, "r");
        if (!f) continue;

        char name[64] = {};
        bool got = fgets(name, sizeof(name), f) != nullptr;
        fclose(f);
        if (!got) continue;

        // Only match our virtual device, not the puck's sub-devices
        if (strncmp(name, "Steam Controller", 16) != 0) continue;
        if (strstr(name, "Puck")) continue;

        int num = atoi(ent->d_name + 5);
        if (num <= newestNum) continue;
        newestNum = num;
        strncpy(newestInput, ent->d_name, sizeof(newestInput) - 1);

        // Find the event sub-node and its major:minor
        char evDir[256];
        snprintf(evDir, sizeof(evDir), "/sys/class/input/%s", ent->d_name);
        DIR* evd = opendir(evDir);
        if (!evd) continue;
        struct dirent* ev;
        while ((ev = readdir(evd)) != nullptr) {
            if (strncmp(ev->d_name, "event", 5) != 0) continue;
            strncpy(newestEvent, ev->d_name, sizeof(newestEvent) - 1);
            char devFile[256];
            snprintf(devFile, sizeof(devFile),
                     "/sys/class/input/%s/%s/dev",
                     ent->d_name, ev->d_name);
            FILE* df = fopen(devFile, "r");
            if (!df) continue;
            (void)fscanf(df, "%d:%d", &major, &minor);
            fclose(df);
        }
        closedir(evd);
    }
    closedir(dir);

    if (newestEvent[0] == '\0') return;

    char node[256];
    snprintf(node, sizeof(node), "/dev/input/%s", newestEvent);
    struct stat st;
    if (stat(node, &st) == 0) return;   // already exists

    dev_t devno = makedev(major, minor);
    if (mknod(node, S_IFCHR | 0666, devno) == 0) {
        chmod(node, 0666);
        // Restore SELinux context on Android
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
                 "/system/bin/restorecon %s 2>/dev/null", node);
        (void)system(cmd);
        printf("  fixed event node %s (%d:%d)\n", node, major, minor);
    }
}

// ---------------------------------------------------------------------------
// Create / Destroy
// ---------------------------------------------------------------------------

VirtualGamepad::VirtualGamepad()
{
    memset(m_prevButtons, 0, sizeof(m_prevButtons));
    memset(m_effects, 0, sizeof(m_effects));
}

bool VirtualGamepad::Create()
{
    // O_RDWR — needed so we can read EV_FF events back from kernel for rumble
    m_fd = open("/dev/uinput", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (m_fd < 0) { perror("  uinput open"); return false; }

    // ---- Buttons ----
    ioctl(m_fd, UI_SET_EVBIT, EV_KEY);
    for (int key : {
            BTN_A, BTN_B, BTN_X, BTN_Y,
            BTN_TL, BTN_TR,
            BTN_THUMBL, BTN_THUMBR,
            BTN_START, BTN_SELECT, BTN_MODE,
            BTN_TL2, BTN_TR2,           // L4, R4
            BTN_TRIGGER_HAPPY1,         // L5
            BTN_TRIGGER_HAPPY2,         // R5
            BTN_EXTRA,                  // trackpad click
        })
        ioctl(m_fd, UI_SET_KEYBIT, key);

    // ---- Absolute axes ----
    ioctl(m_fd, UI_SET_EVBIT, EV_ABS);
    for (int ax : {
            ABS_X, ABS_Y,               // left stick
            ABS_RX, ABS_RY,             // right stick  ← fixed from ABS_Z/ABS_RZ
            ABS_BRAKE, ABS_GAS,         // triggers
            ABS_HAT0X, ABS_HAT0Y,       // d-pad
            // Trackpads via MT
            ABS_MT_SLOT, ABS_MT_TRACKING_ID,
            ABS_MT_POSITION_X, ABS_MT_POSITION_Y,
            ABS_MT_TOUCH_MAJOR,
        })
        ioctl(m_fd, UI_SET_ABSBIT, ax);

    // ---- Force feedback ----
    ioctl(m_fd, UI_SET_EVBIT, EV_FF);
    ioctl(m_fd, UI_SET_FFBIT, FF_RUMBLE);
    ioctl(m_fd, UI_SET_FFBIT, FF_PERIODIC);

    // ---- Sync ----
    ioctl(m_fd, UI_SET_EVBIT, EV_SYN);

    // ---- Device descriptor ----
    struct uinput_setup usetup{};
    usetup.id.bustype    = BUS_USB;
    usetup.id.vendor     = 0x28DE;
    usetup.id.product    = 0x1304;
    usetup.id.version    = 1;
    usetup.ff_effects_max = SC2_FF_MAX;
    strcpy(usetup.name, "Steam Controller 2");

    auto setupAbs = [&](int code, int min, int max, int flat, int fuzz) {
        struct uinput_abs_setup abs{};
        abs.code = code;
        abs.absinfo.minimum = min;
        abs.absinfo.maximum = max;
        abs.absinfo.flat    = flat;
        abs.absinfo.fuzz    = fuzz;
        ioctl(m_fd, UI_ABS_SETUP, &abs);
    };

    setupAbs(ABS_X,      -32768, 32767, 512, 16);
    setupAbs(ABS_Y,      -32768, 32767, 512, 16);
    setupAbs(ABS_RX,     -32768, 32767, 512, 16);
    setupAbs(ABS_RY,     -32768, 32767, 512, 16);
    setupAbs(ABS_BRAKE,       0, 32767,   0,  0);
    setupAbs(ABS_GAS,         0, 32767,   0,  0);
    setupAbs(ABS_HAT0X,      -1,     1,   0,  0);
    setupAbs(ABS_HAT0Y,      -1,     1,   0,  0);
    setupAbs(ABS_MT_SLOT,              0,     1,   0,  0);
    setupAbs(ABS_MT_TRACKING_ID,       0, 65535,   0,  0);
    setupAbs(ABS_MT_POSITION_X,        0, 65535,   0,  0);
    setupAbs(ABS_MT_POSITION_Y,        0, 65535,   0,  0);
    setupAbs(ABS_MT_TOUCH_MAJOR,       0, 65535,   0,  0);

    if (ioctl(m_fd, UI_DEV_SETUP, &usetup) < 0) {
        perror("  UI_DEV_SETUP"); close(m_fd); m_fd = -1; return false;
    }
    if (ioctl(m_fd, UI_DEV_CREATE) < 0) {
        perror("  UI_DEV_CREATE"); close(m_fd); m_fd = -1; return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    FixEventNode();

    printf("  virtual gamepad created\n");
    return true;
}

void VirtualGamepad::Destroy()
{
    if (m_fd >= 0) {
        ioctl(m_fd, UI_DEV_DESTROY);
        close(m_fd);
        m_fd = -1;
    }
}

// ---------------------------------------------------------------------------
// Force feedback — poll for FF events from the kernel
// Returns true + fills strong/weak if a rumble play event was received.
// ---------------------------------------------------------------------------

bool VirtualGamepad::PollFF(uint16_t& outStrong, uint16_t& outWeak)
{
    if (m_fd < 0) return false;

    struct input_event ev{};
    ssize_t n = read(m_fd, &ev, sizeof(ev));
    if (n != sizeof(ev)) return false;

    if (ev.type == EV_UINPUT) {
        if (ev.code == UI_FF_UPLOAD) {
            struct uinput_ff_upload up{};
            up.request_id = ev.value;
            ioctl(m_fd, UI_BEGIN_FF_UPLOAD, &up);
            if (up.effect.id >= 0 && up.effect.id < SC2_FF_MAX)
                m_effects[up.effect.id] = up.effect;
            up.retval = 0;
            ioctl(m_fd, UI_END_FF_UPLOAD, &up);

        } else if (ev.code == UI_FF_ERASE) {
            struct uinput_ff_erase er{};
            er.request_id = ev.value;
            ioctl(m_fd, UI_BEGIN_FF_ERASE, &er);
            if ((int)er.effect_id < SC2_FF_MAX)
                memset(&m_effects[er.effect_id], 0, sizeof(m_effects[0]));
            er.retval = 0;
            ioctl(m_fd, UI_END_FF_ERASE, &er);
        }
        return false;
    }

    if (ev.type == EV_FF) {
        int id = ev.code;
        if (id < 0 || id >= SC2_FF_MAX) return false;

        if (ev.value == 0) {
            // Stop
            outStrong = outWeak = 0;
            return true;
        }

        auto& eff = m_effects[id];
        if (eff.type == FF_RUMBLE) {
            outStrong = eff.u.rumble.strong_magnitude;
            outWeak   = eff.u.rumble.weak_magnitude;
            return true;
        }
        if (eff.type == FF_PERIODIC) {
            outStrong = outWeak = eff.u.periodic.magnitude;
            return true;
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// Update — parse 0x42 report and emit input events
// ---------------------------------------------------------------------------

void VirtualGamepad::Update(const uint8_t* buf, size_t n)
{
    if (n < 30 || buf[0] != SteamController::REPORT_STATE)
        return;

    // ---- Buttons byte 0 (buf[2]) ----
    if (buf[2] != m_prevButtons[0]) {
        uint8_t d = buf[2] ^ m_prevButtons[0];
        if (d & SteamController::SC_BTN_A)    Emit(EV_KEY, BTN_A,       buf[2] & SteamController::SC_BTN_A    ? 1 : 0);
        if (d & SteamController::SC_BTN_B)    Emit(EV_KEY, BTN_B,       buf[2] & SteamController::SC_BTN_B    ? 1 : 0);
        if (d & SteamController::SC_BTN_X)    Emit(EV_KEY, BTN_X,       buf[2] & SteamController::SC_BTN_X    ? 1 : 0);
        if (d & SteamController::SC_BTN_Y)    Emit(EV_KEY, BTN_Y,       buf[2] & SteamController::SC_BTN_Y    ? 1 : 0);
        if (d & SteamController::SC_BTN_RS)   Emit(EV_KEY, BTN_THUMBR,  buf[2] & SteamController::SC_BTN_RS   ? 1 : 0);
        if (d & SteamController::SC_BTN_MENU) Emit(EV_KEY, BTN_START,   buf[2] & SteamController::SC_BTN_MENU ? 1 : 0);
        if (d & SteamController::SC_BTN_R4)   Emit(EV_KEY, BTN_TR2,     buf[2] & SteamController::SC_BTN_R4   ? 1 : 0);
        m_prevButtons[0] = buf[2];
    }

    // ---- Buttons byte 1 (buf[3]) ----
    if (buf[3] != m_prevButtons[1]) {
        uint8_t d = buf[3] ^ m_prevButtons[1];
        if (d & SteamController::SC_BTN_R5)   Emit(EV_KEY, BTN_TRIGGER_HAPPY2, buf[3] & SteamController::SC_BTN_R5   ? 1 : 0);
        if (d & SteamController::SC_BTN_RB)   Emit(EV_KEY, BTN_TR,             buf[3] & SteamController::SC_BTN_RB   ? 1 : 0);
        if (d & SteamController::SC_BTN_VIEW) Emit(EV_KEY, BTN_SELECT,         buf[3] & SteamController::SC_BTN_VIEW ? 1 : 0);
        if (d & SteamController::SC_BTN_LS)   Emit(EV_KEY, BTN_THUMBL,         buf[3] & SteamController::SC_BTN_LS   ? 1 : 0);
        m_prevButtons[1] = buf[3];
    }

    // ---- Buttons byte 2 (buf[4]) ----
    if (buf[4] != m_prevButtons[2]) {
        uint8_t d = buf[4] ^ m_prevButtons[2];
        if (d & SteamController::SC_BTN_STEAM) Emit(EV_KEY, BTN_MODE,           buf[4] & SteamController::SC_BTN_STEAM ? 1 : 0);
        if (d & SteamController::SC_BTN_L4)    Emit(EV_KEY, BTN_TL2,            buf[4] & SteamController::SC_BTN_L4   ? 1 : 0);
        if (d & SteamController::SC_BTN_L5)    Emit(EV_KEY, BTN_TRIGGER_HAPPY1, buf[4] & SteamController::SC_BTN_L5   ? 1 : 0);
        if (d & SteamController::SC_BTN_LB)    Emit(EV_KEY, BTN_TL,             buf[4] & SteamController::SC_BTN_LB   ? 1 : 0);
        m_prevButtons[2] = buf[4];
    }

    // ---- D-pad ----
    int hatX = 0, hatY = 0;
    if      (buf[3] & SteamController::SC_DPAD_LT) hatX = -1;
    else if (buf[3] & SteamController::SC_DPAD_RT) hatX =  1;
    if      (buf[3] & SteamController::SC_DPAD_UP) hatY = -1;
    else if (buf[3] & SteamController::SC_DPAD_DN) hatY =  1;
    if (hatX != m_prevHatX) { Emit(EV_ABS, ABS_HAT0X, hatX); m_prevHatX = hatX; }
    if (hatY != m_prevHatY) { Emit(EV_ABS, ABS_HAT0Y, hatY); m_prevHatY = hatY; }

    // ---- Flags: trackpad click ----
    if ((buf[5] ^ m_prevFlags) & SteamController::SC_TP_LT_CLICK)
        Emit(EV_KEY, BTN_EXTRA, buf[5] & SteamController::SC_TP_LT_CLICK ? 1 : 0);
    m_prevFlags = buf[5];

    // ---- Triggers ----
    int32_t tL = Read16LE(buf, 6); if (tL < 0) tL = 0;
    int32_t tR = Read16LE(buf, 8); if (tR < 0) tR = 0;
    if (tL != m_prevTriggerL) { Emit(EV_ABS, ABS_BRAKE, tL); m_prevTriggerL = tL; }
    if (tR != m_prevTriggerR) { Emit(EV_ABS, ABS_GAS,   tR); m_prevTriggerR = tR; }

    // ---- Sticks ----
    int32_t lx =  Read16LE(buf, 10);
    int32_t ly = -Read16LE(buf, 12);   // firmware +Y = up, Linux +Y = down
    int32_t rx =  Read16LE(buf, 14);
    int32_t ry = -Read16LE(buf, 16);
    if (lx != m_prevLX) { Emit(EV_ABS, ABS_X,  lx); m_prevLX = lx; }
    if (ly != m_prevLY) { Emit(EV_ABS, ABS_Y,  ly); m_prevLY = ly; }
    if (rx != m_prevRX) { Emit(EV_ABS, ABS_RX, rx); m_prevRX = rx; }
    if (ry != m_prevRY) { Emit(EV_ABS, ABS_RY, ry); m_prevRY = ry; }

    // ---- Trackpads (MT protocol) ----
    int16_t tp1x = Read16LE(buf, 18);
    int16_t tp1y = Read16LE(buf, 20);
    int32_t tp1c = Read16LE(buf, 22);  // contact area (already 0..32767)
    int16_t tp2x = Read16LE(buf, 24);
    int16_t tp2y = Read16LE(buf, 26);
    int32_t tp2c = Read16LE(buf, 28);

    bool touching1 = (buf[5] & SteamController::SC_BTN_TP_LT) != 0;
    bool touching2 = (buf[4] & SteamController::SC_BTN_TP_RT) != 0;

    // Slot 0 — left trackpad
    Emit(EV_ABS, ABS_MT_SLOT, 0);
    if (touching1 && m_tp1TrackingId < 0) {
        m_tp1TrackingId = 0;
        Emit(EV_ABS, ABS_MT_TRACKING_ID, 0);
    } else if (!touching1 && m_tp1TrackingId >= 0) {
        Emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
        m_tp1TrackingId = -1;
    }
    if (touching1) {
        int32_t mx = RemapToU16(tp1x);
        int32_t my = RemapToU16(tp1y);
        if (mx != m_prevTP1X) { Emit(EV_ABS, ABS_MT_POSITION_X,   mx);  m_prevTP1X = mx; }
        if (my != m_prevTP1Y) { Emit(EV_ABS, ABS_MT_POSITION_Y,   my);  m_prevTP1Y = my; }
        if (tp1c != m_prevTP1C){ Emit(EV_ABS, ABS_MT_TOUCH_MAJOR, tp1c); m_prevTP1C = tp1c; }
    }

    // Slot 1 — right trackpad
    Emit(EV_ABS, ABS_MT_SLOT, 1);
    if (touching2 && m_tp2TrackingId < 0) {
        m_tp2TrackingId = 1;
        Emit(EV_ABS, ABS_MT_TRACKING_ID, 1);
    } else if (!touching2 && m_tp2TrackingId >= 0) {
        Emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
        m_tp2TrackingId = -1;
    }
    if (touching2) {
        int32_t mx = RemapToU16(tp2x);
        int32_t my = RemapToU16(tp2y);
        if (mx != m_prevTP2X) { Emit(EV_ABS, ABS_MT_POSITION_X,   mx);  m_prevTP2X = mx; }
        if (my != m_prevTP2Y) { Emit(EV_ABS, ABS_MT_POSITION_Y,   my);  m_prevTP2Y = my; }
        if (tp2c != m_prevTP2C){ Emit(EV_ABS, ABS_MT_TOUCH_MAJOR, tp2c); m_prevTP2C = tp2c; }
    }

    // Reset slot back to 0
    Emit(EV_ABS, ABS_MT_SLOT, 0);

    EmitSyn();
}
