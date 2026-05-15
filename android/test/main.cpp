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
    printf("  ║   sc2d  —  Steam Controller              ║\n");
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

        if (!controller.Open()) {
            printf("  retrying in 3s...\n");
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }

        printf("  disabling lizard mode...\n");
        if (!controller.DisableLizardMode()) {
            fprintf(stderr, "  FAILED to disable lizard mode\n");
            controller.Close();
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }
        printf("  lizard mode OFF\n");

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
        printf("  running — hold Steam+Menu+View 1s to stop\n\n");

        uint8_t buf[64];
        bool comboWasActive = false;
        auto comboStart = std::chrono::steady_clock::time_point{};
        uint16_t rumbleStrong = 0, rumbleWeak = 0;

        while (g_running.load() && controller.IsOpen()) {

            size_t n = controller.ReadReport(buf, sizeof(buf), 8);

            if (n > 0 && buf[0] == SteamController::REPORT_STATE) {
                gamepad.Update(buf, n);

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

            // Rumble — always update from PollFF, send on every loop if active
            uint16_t s, w;
            if (gamepad.PollFF(s, w)) {
                rumbleStrong = s;
                rumbleWeak = w;
            } else {
                rumbleStrong = 0;
                rumbleWeak = 0;
            }
            if (rumbleStrong || rumbleWeak)
                controller.SendHaptic(rumbleStrong, rumbleWeak);
        }

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
