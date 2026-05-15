#include "SteamController.h"
#include "VirtualGamepad.h"

#ifdef HAS_PASSTHROUGH
#include "PassthroughDevice.h"
#endif

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
#ifdef HAS_PASSTHROUGH
    PassthroughDevice passthrough;
#endif

    // ---- Create virtual gamepad ONCE (persists across disconnects) ----
    printf("  creating virtual gamepad...\n");
    while (g_running.load()) {
        if (gamepad.Create())
            break;
        printf("  retrying gamepad creation in 3s...\n");
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }
    if (!g_running.load()) return 0;
    printf("  virtual gamepad ready (persistent)\n\n");

#ifdef HAS_PASSTHROUGH
    passthrough.Open(gamepad.GetUinputFd());
#endif

    printf("  daemon running — tap Steam+Menu+View 3× to stop\n\n");

    uint8_t buf[64];
    bool comboWasPressed = false;
    int  tapCount = 0;
    auto lastTapTime = std::chrono::steady_clock::now();

    while (g_running.load()) {

        // ---- Connect / Reconnect ----
        if (!controller.IsOpen()) {
            if (!controller.Open()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }

            printf("  disabling lizard mode...\n");
            if (!controller.DisableLizardMode()) {
                fprintf(stderr, "  FAILED to disable lizard mode\n");
                controller.Close();
                std::this_thread::sleep_for(std::chrono::seconds(3));
                continue;
            }
            printf("  lizard mode OFF — controller connected\n");
        }

        // ---- Read input ----
        size_t n = controller.ReadReport(buf, sizeof(buf), 100);

        if (n == 0) {
#ifdef HAS_PASSTHROUGH
            passthrough.Poll();
#endif
            if (!controller.IsOpen())
                printf("  controller disconnected, gamepad stays alive\n");
            continue;
        }

        if (buf[0] != SteamController::REPORT_STATE)
            continue;

        gamepad.Update(buf, n);

#ifdef HAS_PASSTHROUGH
        passthrough.Poll();
#endif

        // ---- Triple-tap: press Steam+Menu+View together 3× within 1.5s ----
        bool steam = (buf[4] & SteamController::SC_BTN_STEAM) != 0;
        bool menu  = (buf[2] & SteamController::SC_BTN_MENU)  != 0;
        bool view  = (buf[3] & SteamController::SC_BTN_VIEW)  != 0;
        bool allPressed = steam && menu && view;

        if (allPressed && !comboWasPressed) {
            auto now = std::chrono::steady_clock::now();
            auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now - lastTapTime).count();
            if (dt > 1500) tapCount = 0;
            tapCount++;
            lastTapTime = now;
            printf("  tap %d/3\n", tapCount);
            if (tapCount >= 3) {
                printf("  triple tap detected — stopping daemon\n");
                g_running.store(false);
            }
        }
        comboWasPressed = allPressed;

        // L5 + R5 held 0.5s → Android Back
        {
            static bool s_comboWas = false;
            static auto s_start = std::chrono::steady_clock::time_point{};
            static bool s_fired = false;
            bool l5 = (buf[4] & SteamController::SC_BTN_L5) != 0;
            bool r5 = (buf[3] & SteamController::SC_BTN_R5) != 0;
            bool combo = l5 && r5;

            if (combo) {
                if (!s_comboWas)
                    s_start = std::chrono::steady_clock::now();
                auto held = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - s_start).count();
                if (held >= 500 && !s_fired) {
                    s_fired = true;
                    printf("  L5+R5 → Android Back\n");
                    system("input keyevent 4");
                }
            } else if (!l5 && !r5) {
                s_fired = false;
            }
            s_comboWas = combo;
        }
    }

    // ---- Cleanup ----
    printf("\n  cleaning up...\n");
    gamepad.ReleaseAll();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    gamepad.Destroy();
    SteamController::ReleaseStuckKeys();

    if (controller.IsOpen()) {
        uint8_t flush[64];
        for (int i = 0; i < 20; ++i) {
            if (controller.ReadReport(flush, sizeof(flush), 10) == 0) break;
        }
        controller.DrainUntilReleased(
            SteamController::SC_BTN_MENU,
            SteamController::SC_BTN_VIEW,
            SteamController::SC_BTN_STEAM,
            4000);
        controller.EnableLizardMode();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    controller.Close();

    return 0;
}
