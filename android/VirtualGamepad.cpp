#include "VirtualGamepad.h"
#include "SteamController.h"

#include <cstdio>
#include <cstring>
#include <chrono>
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
    write(m_fd, &ev, sizeof(ev));
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

// ---------------------------------------------------------------------------
// Remove stale event nodes from previous runs
// ---------------------------------------------------------------------------

static void CleanupStaleNodes()
{
    int removed = 0;
    DIR* dir = opendir("/sys/class/input");
    if (!dir) return;

    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (strncmp(ent->d_name, "input", 5) != 0) continue;
        char p[256];
        snprintf(p, sizeof(p), "/sys/class/input/%s/name", ent->d_name);
        FILE* f = fopen(p, "r");
        if (!f) continue;
        char n[64];
        // Match our virtual uinput device only, not dongle sub-devices
        // (dongle names start with "Valve Software Steam Controller Puck ...")
        fclose(f);
        if (n[0] != 'S' || strncmp(n, "Steam Controller", 16) != 0)
            continue;
        // Also skip if it has "Puck" (dongle interface)
        if (strstr(n, "Puck")) continue;

        char evPath[256];
        snprintf(evPath, sizeof(evPath), "/sys/class/input/%s", ent->d_name);
        DIR* evd = opendir(evPath);
        if (!evd) continue;
        struct dirent* ev;
        while ((ev = readdir(evd)) != nullptr) {
            if (strncmp(ev->d_name, "event", 5) != 0) continue;
            char node[256];
            snprintf(node, sizeof(node), "/dev/input/%s", ev->d_name);
            if (unlink(node) == 0) removed++;
        }
        closedir(evd);
    }
    closedir(dir);
    if (removed > 0)
        printf("  removed %d stale event node(s)\n", removed);
}

// ---------------------------------------------------------------------------
// Create / Destroy
// ---------------------------------------------------------------------------

VirtualGamepad::VirtualGamepad()
{
    std::memset(&m_prevButtons, 0, sizeof(m_prevButtons));
}

static void FixEventNode();

bool VirtualGamepad::Create()
{
    CleanupStaleNodes();

    m_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (m_fd < 0) {
        perror("  uinput open");
        return false;
    }

    ioctl(m_fd, UI_SET_EVBIT, EV_KEY);
    ioctl(m_fd, UI_SET_EVBIT, EV_ABS);

    // ---- Buttons ----
    for (int key : { BTN_A, BTN_B, BTN_X, BTN_Y,
                     BTN_TL, BTN_TR,
                     BTN_THUMBL, BTN_THUMBR,
                     BTN_START, BTN_SELECT, BTN_MODE })
        ioctl(m_fd, UI_SET_KEYBIT, key);

    ioctl(m_fd, UI_SET_KEYBIT, BTN_TL2);     // L4
    ioctl(m_fd, UI_SET_KEYBIT, BTN_TR2);     // R4
    ioctl(m_fd, UI_SET_KEYBIT, BTN_WHEEL);   // L5
    ioctl(m_fd, UI_SET_KEYBIT, BTN_GEAR_UP); // R5
    ioctl(m_fd, UI_SET_KEYBIT, BTN_EXTRA);   // trackpad click

    // ---- Absolute axes ----
    for (int ax : { ABS_X, ABS_Y, ABS_Z, ABS_RZ,
                    ABS_BRAKE, ABS_GAS,
                    ABS_HAT0X, ABS_HAT0Y })
        ioctl(m_fd, UI_SET_ABSBIT, ax);

    // Trackpads via MT protocol
    ioctl(m_fd, UI_SET_ABSBIT, ABS_MT_SLOT);
    ioctl(m_fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
    ioctl(m_fd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
    ioctl(m_fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
    ioctl(m_fd, UI_SET_ABSBIT, ABS_MT_TOUCH_MAJOR);

    // ---- Configure device ----
    struct uinput_setup usetup{};
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor  = 0x28DE;
    usetup.id.product = 0x1302;
    usetup.id.version = 1;
    strcpy(usetup.name, "Steam Controller");

    auto setupAbs = [&](int code, int min, int max, int flat, int fuzz) {
        struct uinput_abs_setup abs{};
        abs.code = code;
        abs.absinfo.minimum = min;
        abs.absinfo.maximum = max;
        abs.absinfo.flat    = flat;
        abs.absinfo.fuzz    = fuzz;
        ioctl(m_fd, UI_ABS_SETUP, &abs);
    };

    setupAbs(ABS_X,   -32768, 32767, 128, 0);
    setupAbs(ABS_Y,   -32768, 32767, 128, 0);
    setupAbs(ABS_Z,   -32768, 32767, 128, 0);
    setupAbs(ABS_RZ,  -32768, 32767, 128, 0);
    setupAbs(ABS_BRAKE, 0, 32767,   0, 0);
    setupAbs(ABS_GAS,   0, 32767,   0, 0);
    setupAbs(ABS_HAT0X, -1, 1, 0, 0);
    setupAbs(ABS_HAT0Y, -1, 1, 0, 0);
    setupAbs(ABS_MT_POSITION_X, 0, 65535, 0, 0);
    setupAbs(ABS_MT_POSITION_Y, 0, 65535, 0, 0);
    setupAbs(ABS_MT_TOUCH_MAJOR, 0, 65535, 0, 0);

    if (ioctl(m_fd, UI_DEV_SETUP, &usetup) < 0) {
        perror("  uinput UI_DEV_SETUP");
        close(m_fd);
        m_fd = -1;
        return false;
    }
    if (ioctl(m_fd, UI_DEV_CREATE) < 0) {
        perror("  uinput UI_DEV_CREATE");
        close(m_fd);
        m_fd = -1;
        return false;
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
// Create missing /dev/input/eventX node for the newest device
// ---------------------------------------------------------------------------

static void FixEventNode()
{
    char newest[32] = "";
    char newestEv[32] = "";
    int newestMajor = 0, newestMinor = 0;

    DIR* dir = opendir("/sys/class/input");
    if (!dir) return;

    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (strncmp(ent->d_name, "input", 5) != 0) continue;
        char p[256];
        snprintf(p, sizeof(p), "/sys/class/input/%s/name", ent->d_name);
        FILE* f = fopen(p, "r");
        if (!f) continue;
        char n[64];
        if (!fgets(n, sizeof(n), f)) { fclose(f); continue; }
        fclose(f);
        if (n[0] != 'S' || strncmp(n, "Steam Controller", 16) != 0)
            continue;
        if (strstr(n, "Puck")) continue;

        int num = atoi(ent->d_name + 5);
        int best = atoi(newest + 5);
        if (num <= best) continue;

        strncpy(newest, ent->d_name, sizeof(newest) - 1);

        char evPath[256];
        snprintf(evPath, sizeof(evPath), "/sys/class/input/%s", ent->d_name);
        DIR* evd = opendir(evPath);
        if (!evd) continue;
        struct dirent* ev;
        while ((ev = readdir(evd)) != nullptr) {
            if (strncmp(ev->d_name, "event", 5) != 0) continue;
            strncpy(newestEv, ev->d_name, sizeof(newestEv) - 1);
            char devFile[256];
            snprintf(devFile, sizeof(devFile), "/sys/class/input/%s/%s/dev", ent->d_name, ev->d_name);
            FILE* df = fopen(devFile, "r");
            if (!df) continue;
            if (fscanf(df, "%d:%d", &newestMajor, &newestMinor) != 2) { fclose(df); continue; }
            fclose(df);
        }
        closedir(evd);
    }
    closedir(dir);

    if (newest[0] == 0 || newestEv[0] == 0) return;

    char node[256];
    snprintf(node, sizeof(node), "/dev/input/%s", newestEv);
    struct stat st;
    if (stat(node, &st) == 0) return;

    dev_t devno = makedev(newestMajor, newestMinor);
    if (mknod(node, S_IFCHR | 0666, devno) == 0) {
        chown(node, 0, 0);
        chmod(node, 0666);
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "/system/bin/restorecon %s 2>/dev/null", node);
        system(cmd);
        printf("  fixed event node %s (%d:%d)\n", node, newestMajor, newestMinor);
    }
}

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------

void VirtualGamepad::Update(const uint8_t* buf, size_t n)
{
    if (n < 30 || buf[0] != 0x42)
        return;

    // ---- Buttons (buf[2..4]) ----
    for (int i = 0; i < 3; ++i) {
        if (buf[2 + i] == m_prevButtons[i])
            continue;

        if (i == 0) {
            uint8_t d = buf[2] ^ m_prevButtons[0];
            if (d & SteamController::SC_BTN_A)   Emit(EV_KEY, BTN_A,      buf[2] & SteamController::SC_BTN_A ? 1 : 0);
            if (d & SteamController::SC_BTN_B)   Emit(EV_KEY, BTN_B,      buf[2] & SteamController::SC_BTN_B ? 1 : 0);
            if (d & SteamController::SC_BTN_X)   Emit(EV_KEY, BTN_X,      buf[2] & SteamController::SC_BTN_X ? 1 : 0);
            if (d & SteamController::SC_BTN_Y)   Emit(EV_KEY, BTN_Y,      buf[2] & SteamController::SC_BTN_Y ? 1 : 0);
            if (d & SteamController::SC_BTN_RS)  Emit(EV_KEY, BTN_THUMBR, buf[2] & SteamController::SC_BTN_RS ? 1 : 0);
            if (d & SteamController::SC_BTN_MENU) Emit(EV_KEY, BTN_START, buf[2] & SteamController::SC_BTN_MENU ? 1 : 0);
            if (d & SteamController::SC_BTN_R4)  Emit(EV_KEY, BTN_TR2,    buf[2] & SteamController::SC_BTN_R4 ? 1 : 0);
        }

        if (i == 1) {
            uint8_t d = buf[3] ^ m_prevButtons[1];
            if (d & SteamController::SC_BTN_R5)   Emit(EV_KEY, BTN_GEAR_UP, buf[3] & SteamController::SC_BTN_R5 ? 1 : 0);
            if (d & SteamController::SC_BTN_RB)   Emit(EV_KEY, BTN_TR,      buf[3] & SteamController::SC_BTN_RB ? 1 : 0);
            if (d & SteamController::SC_BTN_VIEW) Emit(EV_KEY, BTN_SELECT,  buf[3] & SteamController::SC_BTN_VIEW ? 1 : 0);
            if (d & SteamController::SC_BTN_LS)   Emit(EV_KEY, BTN_THUMBL,  buf[3] & SteamController::SC_BTN_LS ? 1 : 0);
        }

        if (i == 2) {
            uint8_t d = buf[4] ^ m_prevButtons[2];
            if (d & SteamController::SC_BTN_STEAM) Emit(EV_KEY, BTN_MODE, buf[4] & SteamController::SC_BTN_STEAM ? 1 : 0);
            if (d & SteamController::SC_BTN_L4)    Emit(EV_KEY, BTN_TL2,  buf[4] & SteamController::SC_BTN_L4 ? 1 : 0);
            if (d & SteamController::SC_BTN_L5)    Emit(EV_KEY, BTN_WHEEL, buf[4] & SteamController::SC_BTN_L5 ? 1 : 0);
            if (d & SteamController::SC_BTN_LB)    Emit(EV_KEY, BTN_TL,   buf[4] & SteamController::SC_BTN_LB ? 1 : 0);
        }

        m_prevButtons[i] = buf[2 + i];
    }

    // ---- D-pad as hat switch ----
    int hatX = 0, hatY = 0;
    if (buf[3] & SteamController::SC_DPAD_LT) hatX = -1;
    else if (buf[3] & SteamController::SC_DPAD_RT) hatX = 1;
    if (buf[3] & SteamController::SC_DPAD_UP) hatY = -1;
    else if (buf[3] & SteamController::SC_DPAD_DN) hatY = 1;

    if (hatX != m_prevHatX) { Emit(EV_ABS, ABS_HAT0X, hatX); m_prevHatX = hatX; }
    if (hatY != m_prevHatY) { Emit(EV_ABS, ABS_HAT0Y, hatY); m_prevHatY = hatY; }

    // ---- Flags byte — trackpad click ----
    if ((buf[5] ^ m_prevFlags) & SteamController::SC_TP_LT_CLICK)
        Emit(EV_KEY, BTN_EXTRA, buf[5] & SteamController::SC_TP_LT_CLICK ? 1 : 0);
    m_prevFlags = buf[5];

    // ---- Triggers ----
    int32_t tL = Read16LE(buf, 6);
    int32_t tR = Read16LE(buf, 8);
    if (tL < 0) tL = 0;
    if (tL != m_prevTriggerL) { Emit(EV_ABS, ABS_BRAKE, tL); m_prevTriggerL = tL; }
    if (tR != m_prevTriggerR) { Emit(EV_ABS, ABS_GAS,   tR); m_prevTriggerR = tR; }

    // ---- Joysticks ----
    int32_t lx = Read16LE(buf, 10);
    int32_t ly = -Read16LE(buf, 12);
    int32_t rx = Read16LE(buf, 14);
    int32_t ry = -Read16LE(buf, 16);

    if (lx != m_prevLX) { Emit(EV_ABS, ABS_X,  lx); m_prevLX = lx; }
    if (ly != m_prevLY) { Emit(EV_ABS, ABS_Y,  ly); m_prevLY = ly; }
    if (rx != m_prevRX) { Emit(EV_ABS, ABS_Z,  rx); m_prevRX = rx; }
    if (ry != m_prevRY) { Emit(EV_ABS, ABS_RZ, ry); m_prevRY = ry; }

    // ---- Trackpads via MT protocol (2 slots) ----
    int32_t tp1x = Read16LE(buf, 18);
    int32_t tp1y = Read16LE(buf, 20);
    int32_t tp1c = Read16LE(buf, 22);
    int32_t tp2x = Read16LE(buf, 24);
    int32_t tp2y = Read16LE(buf, 26);
    int32_t tp2c = Read16LE(buf, 28);

    auto remap = [](int32_t v) { return static_cast<int32_t>(static_cast<uint16_t>(v)); };
    bool touching1 = (buf[5] & SteamController::SC_BTN_TP_LT) != 0;
    bool touching2 = (buf[4] & SteamController::SC_BTN_TP_RT) != 0;

    Emit(EV_ABS, ABS_MT_SLOT, 0);
    if (touching1 && m_tp1TrackingId < 0) {
        m_tp1TrackingId = 0;
        Emit(EV_ABS, ABS_MT_TRACKING_ID, m_tp1TrackingId);
    } else if (!touching1 && m_tp1TrackingId >= 0) {
        Emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
        m_tp1TrackingId = -1;
    }
    if (touching1) {
        if (tp1x != m_prevTP1X) { Emit(EV_ABS, ABS_MT_POSITION_X, remap(tp1x)); m_prevTP1X = tp1x; }
        if (tp1y != m_prevTP1Y) { Emit(EV_ABS, ABS_MT_POSITION_Y, remap(tp1y)); m_prevTP1Y = tp1y; }
        if (tp1c != m_prevTP1C) { Emit(EV_ABS, ABS_MT_TOUCH_MAJOR, tp1c); m_prevTP1C = tp1c; }
    }

    Emit(EV_ABS, ABS_MT_SLOT, 1);
    if (touching2 && m_tp2TrackingId < 0) {
        m_tp2TrackingId = 1;
        Emit(EV_ABS, ABS_MT_TRACKING_ID, m_tp2TrackingId);
    } else if (!touching2 && m_tp2TrackingId >= 0) {
        Emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
        m_tp2TrackingId = -1;
    }
    if (touching2) {
        if (tp2x != m_prevTP2X) { Emit(EV_ABS, ABS_MT_POSITION_X, remap(tp2x)); m_prevTP2X = tp2x; }
        if (tp2y != m_prevTP2Y) { Emit(EV_ABS, ABS_MT_POSITION_Y, remap(tp2y)); m_prevTP2Y = tp2y; }
        if (tp2c != m_prevTP2C) { Emit(EV_ABS, ABS_MT_TOUCH_MAJOR, tp2c); m_prevTP2C = tp2c; }
    }

    Emit(EV_ABS, ABS_MT_SLOT, 0);

    EmitSyn();
}
