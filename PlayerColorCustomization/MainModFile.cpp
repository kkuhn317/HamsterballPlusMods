// Ball Tint mod for Hamsterball Plus API — v5
// Uses a background thread to write colors every 16ms, exactly like the
// working bass.dll version. The bass.dll version "lucked out" by writing
// from a separate thread at arbitrary points in the frame — some writes
// land between game logic and GPU render, which is when the colors stick.
//
// HB+ callbacks (onBallUpdate etc.) fire at fixed points that are either
// before the game overwrites colors or after the ball is already rendered.
// A background thread solves this by writing continuously.

#include "HamsterballAPI.h"
#include <windows.h>
#include <stdio.h>

static constexpr DWORD BOARD_COLOR_BASE = 0x3AB0;
static constexpr DWORD BOARD_COLOR_STRIDE = 0x14;
static constexpr DWORD APP_PROFILE_OFFSET = 0x220;
static constexpr DWORD PROFILE_BOARD_OFFSET = 0x0C;
static constexpr DWORD BOARD_VTABLE_MIN = 0x4D0000;
static constexpr DWORD BOARD_VTABLE_MAX = 0x4D2000;
static constexpr DWORD GLOBAL_APP_PTR = 0x5341E0;

class BallTintMod : public HamsterballAPI {
private:
    IModAPI* api = nullptr;
    HANDLE m_thread = NULL;
    volatile bool m_running = true;

    void createColorSlider(const char* id, const char* label, float defaultVal) {
        CustomSlider s(id, label, defaultVal);
        s.lowerBound = 0.0f;
        s.upperBound = 1.0f;
        s.stepSize = 0.05f;
        s.decimalPlaces = 2;
        api->CreateSlider(s, this);
    }

    static bool validateBoard(DWORD board) {
        if (!board || board < 0x10000) return false;
        if (IsBadReadPtr((void*)board, 4)) return false;
        DWORD vtable = *(DWORD*)board;
        return (vtable >= BOARD_VTABLE_MIN && vtable <= BOARD_VTABLE_MAX);
    }

    static void applyColor(DWORD board, int playerIndex, float r, float g, float b) {
        DWORD addr = board + BOARD_COLOR_BASE + (playerIndex * BOARD_COLOR_STRIDE);
        if (IsBadWritePtr((void*)addr, 16)) return;
        *(float*)(addr + 0x00) = r;
        *(float*)(addr + 0x04) = g;
        *(float*)(addr + 0x08) = b;
        *(float*)(addr + 0x0C) = 1.0f;
    }

    // Find board via App->+0x220→+0x0C (proven bass.dll path)
    static DWORD findBoard() {
        DWORD appPtr = *(DWORD*)GLOBAL_APP_PTR;
        if (!appPtr || appPtr < 0x10000) return 0;
        if (IsBadReadPtr((void*)(appPtr + APP_PROFILE_OFFSET), 4)) return 0;
        DWORD profile = *(DWORD*)(appPtr + APP_PROFILE_OFFSET);
        if (!profile || profile < 0x10000) return 0;
        if (IsBadReadPtr((void*)(profile + PROFILE_BOARD_OFFSET), 4)) return 0;
        DWORD board = *(DWORD*)(profile + PROFILE_BOARD_OFFSET);
        if (!board || board < 0x10000) return 0;
        if (!validateBoard(board)) return 0;
        return board;
    }

    // Background thread — writes colors every 16ms, exactly like bass.dll
    static DWORD WINAPI tintThread(LPVOID param) {
        BallTintMod* self = (BallTintMod*)param;
        IModAPI* api = self->api;

        Sleep(3000); // Wait for game to initialize
        printf("[BallTint] Background thread started\n");

        int counter = 0;
        while (self->m_running) {
            Sleep(16); // ~60Hz, same as bass.dll's 30ms

            DWORD board = findBoard();
            if (!board) continue;

            // Apply all 4 player colors
            applyColor(board, 0,
                api->GetSliderState("TINT_P1_R"),
                api->GetSliderState("TINT_P1_G"),
                api->GetSliderState("TINT_P1_B"));
            applyColor(board, 1,
                api->GetSliderState("TINT_P2_R"),
                api->GetSliderState("TINT_P2_G"),
                api->GetSliderState("TINT_P2_B"));
            applyColor(board, 2,
                api->GetSliderState("TINT_P3_R"),
                api->GetSliderState("TINT_P3_G"),
                api->GetSliderState("TINT_P3_B"));
            applyColor(board, 3,
                api->GetSliderState("TINT_P4_R"),
                api->GetSliderState("TINT_P4_G"),
                api->GetSliderState("TINT_P4_B"));

            // Debug print every ~5 seconds (300 iterations * 16ms ≈ 5s)
            counter++;
            if (counter % 300 == 0) {
                float p1r = api->GetSliderState("TINT_P1_R");
                float p1g = api->GetSliderState("TINT_P1_G");
                float p1b = api->GetSliderState("TINT_P1_B");
                printf("[BallTint] thread: board=%08lX P1(%.2f,%.2f,%.2f)\n",
                    (unsigned long)board, p1r, p1g, p1b);
            }
        }
        return 0;
    }

public:
    const char* GetModName() override { return "Ball Tint"; }
    const char* GetAuthorName() override { return "Hamsterbot"; }
    const char* GetContributors() override { return "v5: background thread (bass.dll method)"; }
    int GetApiVersion() override { return HAMSTERBALL_API_VERSION; }

    void Initialize(IModAPI* modApi) override {
        api = modApi;
        printf("[BallTint] Initialize() api=%p\n", (void*)api);

        createColorSlider("TINT_P1_R", "P1 Red", 1.0f);
        createColorSlider("TINT_P1_G", "P1 Green", 1.0f);
        createColorSlider("TINT_P1_B", "P1 Blue", 1.0f);
        createColorSlider("TINT_P2_R", "P2 Red", 0.0f);
        createColorSlider("TINT_P2_G", "P2 Green", 0.5f);
        createColorSlider("TINT_P2_B", "P2 Blue", 1.0f);
        createColorSlider("TINT_P3_R", "P3 Red", 1.0f);
        createColorSlider("TINT_P3_G", "P3 Green", 0.25f);
        createColorSlider("TINT_P3_B", "P3 Blue", 0.25f);
        createColorSlider("TINT_P4_R", "P4 Red", 1.0f);
        createColorSlider("TINT_P4_G", "P4 Green", 1.0f);
        createColorSlider("TINT_P4_B", "P4 Blue", 0.0f);

        // Spawn background thread (same approach as working bass.dll version)
        m_thread = CreateThread(NULL, 0, tintThread, this, 0, NULL);
        printf("[BallTint] Background thread spawned: handle=%p\n", (void*)m_thread);
    }

    ~BallTintMod() {
        m_running = false;
        if (m_thread) {
            WaitForSingleObject(m_thread, 1000);
            CloseHandle(m_thread);
        }
    }
};

extern "C" __declspec(dllexport) HamsterballAPI* CreateModInstance() {
    return new BallTintMod();
}