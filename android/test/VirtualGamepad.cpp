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

// ---------------------------------------------------------------------------
// Remove stale event nodes (our version with Puck exclusion)
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
        if (!fgets(n, sizeof(n), f)) { fclose(f); continue; }
        fclose(f);
        if (n[0] != 'S' || strncmp(n, "Steam Controller", 16) != 0) continue;
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
// Fix missing /dev/input/eventX node (our version with Puck exclusion)
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
        if (n[0] != 'S' || strncmp(n, "Steam Controller", 16) != 0) continue;
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
        chmod(node, 0666);
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "/system/bin/restorecon %s 2>/dev/null", node);
        (void)system(cmd);
        printf("  fixed event node %s (%d:%d)\n", node, newestMajor, newestMinor);
    }
}

// ---------------------------------------------------------------------------
// Create / Destroy
// ---------------------------------------------------------------------------

VirtualGamepad::VirtualGamepad()
{
    std::memset(m_prevButtons, 0, sizeof(m_prevButtons));
    std::memset(m_effects, 0, sizeof(m_effects));
}

bool VirtualGamepad::Create()
{
    CleanupStaleNodes();

    m_fd = open("/dev/uinput", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (m_fd < 0) { perror("  uinput open"); return false; }

    // ---- Buttons ----
    ioctl(m_fd, UI_SET_EVBIT, EV_KEY);
    for (int key : {
            BTN_A, BTN_B, BTN_X, BTN_Y,
            BTN_TL, BTN_TR,
            BTN_THUMBL, BTN_THUMBR,
            BTN_START, BTN_SELECT, BTN_MODE,
            BTN_TL2, BTN_TR2,
            BTN_TRIGGER_HAPPY1,   // L5
            BTN_TRIGGER_HAPPY2,   // R5
            BTN_EXTRA,
        })
        ioctl(m_fd, UI_SET_KEYBIT, key);

    // ---- Axes ----
    ioctl(m_fd, UI_SET_EVBIT, EV_ABS);
    for (int ax : {
            ABS_X, ABS_Y,           // left stick
            ABS_Z, ABS_RZ,          // right stick (Android: AXIS_Z / AXIS_RZ)
            ABS_BRAKE, ABS_GAS,     // triggers
            ABS_HAT0X, ABS_HAT0Y,   // d-pad
            ABS_MT_SLOT, ABS_MT_TRACKING_ID,
            ABS_MT_POSITION_X, ABS_MT_POSITION_Y,
            ABS_MT_TOUCH_MAJOR,
        })
        ioctl(m_fd, UI_SET_ABSBIT, ax);

    // ---- FF ----
    ioctl(m_fd, UI_SET_EVBIT, EV_FF);
    ioctl(m_fd, UI_SET_FFBIT, FF_RUMBLE);
    ioctl(m_fd, UI_SET_EVBIT, EV_SYN);

    // ---- Descriptor ----
    struct uinput_setup usetup{};
    usetup.id.bustype    = BUS_USB;
    usetup.id.vendor     = 0x28DE;
    usetup.id.product    = 0x1302;
    usetup.id.version    = 1;
    usetup.ff_effects_max = SC2_FF_MAX;
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

    setupAbs(ABS_X,      -32768, 32767, 512, 16);
    setupAbs(ABS_Y,      -32768, 32767, 512, 16);
    setupAbs(ABS_Z,      -32768, 32767, 512, 16);
    setupAbs(ABS_RZ,     -32768, 32767, 512, 16);
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
// Force feedback poll
// ---------------------------------------------------------------------------

bool VirtualGamepad::PollFF(uint16_t& outStrong, uint16_t& outWeak)
{
    if (m_fd < 0) return false;

    // Drain all pending FF events
    while (true) {
        struct input_event ev{};
        ssize_t n = read(m_fd, &ev, sizeof(ev));
        if (n != sizeof(ev)) break;

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
                if ((int)er.effect_id < SC2_FF_MAX) {
                    std::memset(&m_effects[er.effect_id], 0, sizeof(m_effects[0]));
                    m_ffActive[er.effect_id] = false;
                }
                er.retval = 0;
                ioctl(m_fd, UI_END_FF_ERASE, &er);
            }
            continue;
        }

        if (ev.type == EV_FF) {
            int id = ev.code;
            if (id < 0 || id >= SC2_FF_MAX) continue;
            if (ev.value != 0) {
                // PLAY — store magnitude and activate
                auto& eff = m_effects[id];
                if (eff.type == FF_RUMBLE || eff.type == FF_PERIODIC)
                    m_ffActive[id] = true;
            } else {
                // STOP — deactivate
                m_ffActive[id] = false;
            }
        }
    }

    // Compute max strong/weak across all active effects
    outStrong = 0;
    outWeak = 0;
    bool any = false;
    for (int i = 0; i < SC2_FF_MAX; i++) {
        if (!m_ffActive[i]) continue;
        auto& eff = m_effects[i];
        if (eff.type == FF_RUMBLE) {
            if (eff.u.rumble.strong_magnitude > outStrong) outStrong = eff.u.rumble.strong_magnitude;
            if (eff.u.rumble.weak_magnitude   > outWeak)   outWeak   = eff.u.rumble.weak_magnitude;
            any = true;
        } else if (eff.type == FF_PERIODIC) {
            if (eff.u.periodic.magnitude > outStrong) outStrong = eff.u.periodic.magnitude;
            if (eff.u.periodic.magnitude > outWeak)   outWeak   = eff.u.periodic.magnitude;
            any = true;
        }
    }
    return any;
}

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------

void VirtualGamepad::Update(const uint8_t* buf, size_t n)
{
    if (n < 30 || buf[0] != SteamController::REPORT_STATE)
        return;

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

    if (buf[3] != m_prevButtons[1]) {
        uint8_t d = buf[3] ^ m_prevButtons[1];
        if (d & SteamController::SC_BTN_R5)   Emit(EV_KEY, BTN_TRIGGER_HAPPY2, buf[3] & SteamController::SC_BTN_R5   ? 1 : 0);
        if (d & SteamController::SC_BTN_RB)   Emit(EV_KEY, BTN_TR,             buf[3] & SteamController::SC_BTN_RB   ? 1 : 0);
        if (d & SteamController::SC_BTN_VIEW) Emit(EV_KEY, BTN_SELECT,         buf[3] & SteamController::SC_BTN_VIEW ? 1 : 0);
        if (d & SteamController::SC_BTN_LS)   Emit(EV_KEY, BTN_THUMBL,         buf[3] & SteamController::SC_BTN_LS   ? 1 : 0);
        m_prevButtons[1] = buf[3];
    }

    if (buf[4] != m_prevButtons[2]) {
        uint8_t d = buf[4] ^ m_prevButtons[2];
        if (d & SteamController::SC_BTN_STEAM) Emit(EV_KEY, BTN_MODE,           buf[4] & SteamController::SC_BTN_STEAM ? 1 : 0);
        if (d & SteamController::SC_BTN_L4)    Emit(EV_KEY, BTN_TL2,            buf[4] & SteamController::SC_BTN_L4   ? 1 : 0);
        if (d & SteamController::SC_BTN_L5)    Emit(EV_KEY, BTN_TRIGGER_HAPPY1, buf[4] & SteamController::SC_BTN_L5   ? 1 : 0);
        if (d & SteamController::SC_BTN_LB)    Emit(EV_KEY, BTN_TL,             buf[4] & SteamController::SC_BTN_LB   ? 1 : 0);
        m_prevButtons[2] = buf[4];
    }

    // D-pad
    int hatX = 0, hatY = 0;
    if      (buf[3] & SteamController::SC_DPAD_LT) hatX = -1;
    else if (buf[3] & SteamController::SC_DPAD_RT) hatX =  1;
    if      (buf[3] & SteamController::SC_DPAD_UP) hatY = -1;
    else if (buf[3] & SteamController::SC_DPAD_DN) hatY =  1;
    if (hatX != m_prevHatX) { Emit(EV_ABS, ABS_HAT0X, hatX); m_prevHatX = hatX; }
    if (hatY != m_prevHatY) { Emit(EV_ABS, ABS_HAT0Y, hatY); m_prevHatY = hatY; }

    // Flags
    if ((buf[5] ^ m_prevFlags) & SteamController::SC_TP_LT_CLICK)
        Emit(EV_KEY, BTN_EXTRA, buf[5] & SteamController::SC_TP_LT_CLICK ? 1 : 0);
    m_prevFlags = buf[5];

    // Triggers
    int32_t tL = Read16LE(buf, 6); if (tL < 0) tL = 0;
    int32_t tR = Read16LE(buf, 8); if (tR < 0) tR = 0;
    if (tL != m_prevTriggerL) { Emit(EV_ABS, ABS_BRAKE, tL); m_prevTriggerL = tL; }
    if (tR != m_prevTriggerR) { Emit(EV_ABS, ABS_GAS,   tR); m_prevTriggerR = tR; }

    // Sticks — right stick on ABS_Z/ABS_RZ (Android native)
    int32_t lx =  Read16LE(buf, 10);
    int32_t ly = -Read16LE(buf, 12);
    int32_t rx =  Read16LE(buf, 14);
    int32_t ry = -Read16LE(buf, 16);
    if (lx != m_prevLX) { Emit(EV_ABS, ABS_X,  lx); m_prevLX = lx; }
    if (ly != m_prevLY) { Emit(EV_ABS, ABS_Y,  ly); m_prevLY = ly; }
    if (rx != m_prevRX) { Emit(EV_ABS, ABS_Z,  rx); m_prevRX = rx; }
    if (ry != m_prevRY) { Emit(EV_ABS, ABS_RZ, ry); m_prevRY = ry; }

    EmitSyn();
}
