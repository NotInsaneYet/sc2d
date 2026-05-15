#include "SteamController.h"
#include "VirtualGamepad.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <thread>

static std::atomic<bool> g_running{true};

static void OnSignal(int sig)
{
    (void)sig;
    printf("\nsc2d: shutting down...\n");
    g_running.store(false);
}

static void PrintBanner()
{
    printf("\n");
    printf("  ╔══════════════════════════════════════════╗\n");
    printf("  ║   sc2d  —  Steam Controller              ║\n");
    printf("  ║   Android userspace daemon               ║\n");
    printf("  ║   Exits lizard mode → virtual gamepad    ║\n");
    printf("  ║   Tap Steam+Menu+View 3× to stop daemon   ║\n");
    printf("  ╚══════════════════════════════════════════╝\n");
    printf("\n");
    printf("  Looking for Steam Controller (VID=28DE PID=1302/1304)...\n\n");
}

int main()
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    signal(SIGINT, OnSignal);
    signal(SIGTERM, OnSignal);

    PrintBanner();

    SteamController controller;
    VirtualGamepad  gamepad;

    while (g_running.load()) {

        // ---- Connect ----
        if (!controller.Open()) {
            printf("  retrying in 3 seconds...\n");
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }

        // ---- Disable lizard mode ----
        printf("  disabling lizard mode...\n");
        if (!controller.DisableLizardMode()) {
            fprintf(stderr, "  FAILED to disable lizard mode\n");
            controller.Close();
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }
        printf("  lizard mode OFF\n");

        // ---- Create virtual gamepad ----
        printf("  creating virtual gamepad...\n");
        if (!gamepad.Create()) {
            fprintf(stderr, "  FAILED to create virtual gamepad\n");
            controller.EnableLizardMode();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            controller.Close();
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }
        printf("  virtual gamepad ready\n\n");
        printf("  daemon running — tap Steam+Menu+View 3× to stop\n\n");

        // ---- Main input loop ----
        uint8_t buf[64];
        bool reportedDisconnect = false;

        // Triple-tap detection: press Steam+Menu+View together 3× within 1.5s
        bool comboWasPressed = false;
        int  tapCount = 0;
        auto lastTapTime = std::chrono::steady_clock::now();

        while (g_running.load() && controller.IsOpen()) {
            size_t n = controller.ReadReport(buf, sizeof(buf), 100);

            if (n == 0) {
                if (!reportedDisconnect && !controller.IsOpen()) {
                    printf("  controller disconnected\n");
                    reportedDisconnect = true;
                }
                continue;
            }

            if (buf[0] != SteamController::REPORT_STATE)
                continue;

            gamepad.Update(buf, n);

            // Detect any button release (falling edge) and flush stuck keys
            static uint8_t s_prevB2 = 0, s_prevB3 = 0, s_prevB4 = 0, s_prevB5 = 0;
            bool anyReleased = false;
            auto checkRelease = [&](uint8_t cur, uint8_t& prev) {
                if ((prev & ~cur) != 0) anyReleased = true;
                prev = cur;
            };
            checkRelease(buf[2], s_prevB2);
            checkRelease(buf[3], s_prevB3);
            checkRelease(buf[4], s_prevB4);
            checkRelease(buf[5], s_prevB5);
            if (anyReleased)
                SteamController::ReleaseStuckKeys();

            // Debug: print raw button bytes on first run to verify mapping
            static bool s_dumpedMapping = false;
            if (!s_dumpedMapping) {
                bool facePressed = (buf[2] & 0x0F) != 0;
                if (facePressed) {
                    printf("  raw button bytes: [2]=0x%02X [3]=0x%02X [4]=0x%02X\n",
                           buf[2], buf[3], buf[4]);
                    auto showBit = [&](const char* label, uint8_t mask) {
                        printf("    %s: %s\n", label,
                               (buf[2] & mask) ? "PRESSED" : "released");
                    };
                    showBit("A (bit0)", 0x01);
                    showBit("B (bit1)", 0x02);
                    showBit("X (bit2)", 0x04);
                    showBit("Y (bit3)", 0x08);
                    printf("\n");
                    s_dumpedMapping = true;
                }
            }

            // Triple-tap: press Steam+Menu+View together 3× within 1.5s
            bool steam = (buf[4] & SteamController::SC_BTN_STEAM) != 0;
            bool menu  = (buf[2] & SteamController::SC_BTN_MENU)  != 0;
            bool view  = (buf[3] & SteamController::SC_BTN_VIEW)  != 0;
            bool allPressed = steam && menu && view;

            if (allPressed && !comboWasPressed) {
                // Rising edge — user pressed all three
                auto now = std::chrono::steady_clock::now();
                auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                              now - lastTapTime).count();
                if (dt > 1500)
                    tapCount = 0;

                tapCount++;
                lastTapTime = now;
                printf("  tap %d/3\n", tapCount);

                if (tapCount >= 3) {
                    printf("  triple tap detected — stopping daemon\n");
                    g_running.store(false);
                    break;
                }
            }
            comboWasPressed = allPressed;
        }

        // ---- Cleanup this session ----
        printf("\n  shutting down...\n");

        // Release gamepad buttons before destroying the uinput device
        gamepad.ReleaseAll();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        gamepad.Destroy();

        // Flush any stuck keyboard keys directly on the controller's event
        // devices (the temporary uinput approach can't reach these).
        SteamController::ReleaseStuckKeys();

        if (controller.IsOpen()) {
            // Device still connected — this was a combo shutdown.
            // Read and discard pending reports to flush the HID pipe before
            // draining, so we don't see stale combo-held reports.
            {
                uint8_t flush[64];
                for (int i = 0; i < 20; ++i) {
                    if (controller.ReadReport(flush, sizeof(flush), 10) == 0)
                        break;
                }
            }

            // Then wait for the combo buttons to be physically released.
            // This prevents the controller from sending stuck keyboard scancodes
            // when lizard mode re-enables.
            controller.DrainUntilReleased(
                SteamController::SC_BTN_MENU,   // byte 2
                SteamController::SC_BTN_VIEW,   // byte 3
                SteamController::SC_BTN_STEAM,  // byte 4
                4000);

            // Now safe — all buttons are released before lizard mode comes back
            controller.EnableLizardMode();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }

        controller.Close();

        if (g_running.load()) {
            printf("  reconnecting...\n\n");
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }

    return 0;
}
