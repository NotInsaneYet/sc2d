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
    printf("  ║   Hold Steam+Menu+View to stop daemon    ║\n");
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
        printf("  daemon running — hold Steam+Menu+View 1s to stop\n\n");

        // ---- Main input loop ----
        uint8_t buf[64];
        bool reportedDisconnect = false;

        bool comboWasActive = false;
        auto comboStart = std::chrono::steady_clock::time_point{};

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

            // Combo: hold Steam + Menu + View for 1s → kill daemon
            bool steam = (buf[4] & SteamController::SC_BTN_STEAM) != 0;
            bool menu  = (buf[2] & SteamController::SC_BTN_MENU)  != 0;
            bool view  = (buf[3] & SteamController::SC_BTN_VIEW)  != 0;
            bool comboActive = steam && menu && view;

            if (comboActive) {
                if (!comboWasActive)
                    comboStart = std::chrono::steady_clock::now();
                auto held = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - comboStart);
                if (held.count() >= 1000) {
                    printf("  combo detected — stopping daemon\n");
                    raise(SIGTERM);
                }
            }
            comboWasActive = comboActive;
        }

        // ---- Cleanup this session ----
        printf("\n  controller lost — cleaning up...\n");
        gamepad.Destroy();
        controller.EnableLizardMode();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        controller.Close();

        if (g_running.load()) {
            printf("  reconnecting...\n\n");
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }

    return 0;
}
