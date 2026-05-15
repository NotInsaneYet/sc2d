#include "SteamController.h"
#include "VirtualGamepad.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <thread>

static std::atomic<bool> g_running{true};

static void OnSignal(int) { printf("\nsc2d: shutting down...\n"); g_running = false; }

static void PrintBanner()
{
    printf("\n");
    printf("  ╔══════════════════════════════════════════╗\n");
    printf("  ║   sc2d  —  Steam Controller 2            ║\n");
    printf("  ║   Android userspace daemon               ║\n");
    printf("  ║   Exits lizard mode → virtual gamepad    ║\n");
    printf("  ║   Hold Steam+Menu+View 1s to stop        ║\n");
    printf("  ╚══════════════════════════════════════════╝\n\n");
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    signal(SIGINT,  OnSignal);
    signal(SIGTERM, OnSignal);

    PrintBanner();

    SteamController controller;
    VirtualGamepad  gamepad;

    while (g_running.load()) {

        // ---- Connect ----
        printf("  looking for Steam Controller 2...\n");
        if (!controller.Open()) {
            printf("  retrying in 3s...\n");
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }

        // ---- Lizard mode off ----
        printf("  disabling lizard mode...\n");
        if (!controller.DisableLizardMode()) {
            fprintf(stderr, "  FAILED to disable lizard mode\n");
            controller.Close();
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }
        printf("  lizard mode OFF\n");

        // ---- Virtual gamepad ----
        printf("  creating virtual gamepad...\n");
        if (!gamepad.Create()) {
            fprintf(stderr, "  FAILED to create virtual gamepad\n");
            controller.EnableLizardMode();
            controller.Close();
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }
        printf("  virtual gamepad ready\n\n");
        printf("  running — hold Steam+Menu+View 1s to stop\n\n");

        // ---- Main input loop ----
        uint8_t buf[64];
        bool comboWasActive = false;
        auto comboStart = std::chrono::steady_clock::time_point{};

        while (g_running.load() && controller.IsOpen()) {

            // --- Read controller ---
            size_t n = controller.ReadReport(buf, sizeof(buf), /*timeoutMs=*/8);

            if (n > 0 && buf[0] == SteamController::REPORT_STATE) {
                gamepad.Update(buf, n);

                // --- Stop combo: Steam + Menu + View held 1s ---
                bool steam = (buf[4] & SteamController::SC_BTN_STEAM) != 0;
                bool menu  = (buf[2] & SteamController::SC_BTN_MENU)  != 0;
                bool view  = (buf[3] & SteamController::SC_BTN_VIEW)  != 0;
                bool combo = steam && menu && view;

                if (combo) {
                    if (!comboWasActive)
                        comboStart = std::chrono::steady_clock::now();
                    auto held = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - comboStart);
                    if (held.count() >= 1000) {
                        printf("  combo — stopping\n");
                        raise(SIGTERM);
                    }
                }
                comboWasActive = combo;
            }

            if (n == 0 && !controller.IsOpen()) {
                printf("  controller disconnected\n");
                break;
            }

            // --- Poll uinput for FF events and forward to haptics ---
            uint16_t strong = 0, weak = 0;
            if (gamepad.PollFF(strong, weak))
                controller.SendHaptic(strong, weak);
        }

        // ---- Cleanup ----
        printf("\n  cleaning up...\n");
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
