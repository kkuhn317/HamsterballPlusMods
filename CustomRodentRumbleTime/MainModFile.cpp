#include "HamsterballAPI.h"
#include <windows.h>

static constexpr DWORD APP_PROFILE_OFFSET = 0x220;
static constexpr DWORD PROFILE_BOARD_OFFSET = 0x0C;
static constexpr DWORD BOARD_VTABLE_MIN = 0x4D0000;
static constexpr DWORD BOARD_VTABLE_MAX = 0x4D2000;
static constexpr DWORD GLOBAL_APP_PTR = 0x5341E0;
static constexpr DWORD TIMER_OFFSET = 0x47AC;

class ArenaTimerMod : public HamsterballAPI {
private:
    IModAPI* api = nullptr;
    HANDLE m_thread = NULL;
    volatile bool m_running = true;

    void createSlider(const char* id, const char* label, int defaultVal, int minVal, int maxVal) {
        CustomSlider s(id, label, (float)defaultVal);
        s.lowerBound = (float)minVal;
        s.upperBound = (float)maxVal;
        s.stepSize = 100.0f;
        s.decimalPlaces = 0;
        api->CreateSlider(s, this);
    }

    static DWORD findBoard() {
        DWORD appPtr = *(DWORD*)GLOBAL_APP_PTR;
        if (!appPtr || appPtr < 0x10000) return 0;
        if (IsBadReadPtr((void*)(appPtr + APP_PROFILE_OFFSET), 4)) return 0;
        DWORD profile = *(DWORD*)(appPtr + APP_PROFILE_OFFSET);
        if (!profile || profile < 0x10000) return 0;
        if (IsBadReadPtr((void*)(profile + PROFILE_BOARD_OFFSET), 4)) return 0;
        DWORD board = *(DWORD*)(profile + PROFILE_BOARD_OFFSET);
        if (!board || board < 0x10000) return 0;
        if (IsBadReadPtr((void*)board, 4)) return 0;
        DWORD vtable = *(DWORD*)board;
        if (vtable < BOARD_VTABLE_MIN || vtable > BOARD_VTABLE_MAX) return 0;
        return board;
    }

    static DWORD WINAPI timerThread(LPVOID param) {
        ArenaTimerMod* self = (ArenaTimerMod*)param;
        IModAPI* api = self->api;

        Sleep(3000);

        while (self->m_running) {
            Sleep(16);
            DWORD board = findBoard();
            if (board) {
                int timerVal = (int)api->GetSliderState("ARENA_TIMER");
                if (timerVal < 100) timerVal = 100;
                DWORD addr = board + TIMER_OFFSET;
                if (!IsBadReadPtr((void*)addr, 4)) {
                    int current = *(int*)addr;
                    if (current == 6000) {
                        *(int*)addr = timerVal;
                    }
                }
            }
        }
        return 0;
    }

public:
    const char* GetModName() override { return "Arena Timer"; }
    const char* GetAuthorName() override { return "BookwormKevin"; }
    const char* GetContributors() override { return "Hamsterbot"; }
    int GetApiVersion() override { return HAMSTERBALL_API_VERSION; }

    void Initialize(IModAPI* modApi) override {
        api = modApi;

        createSlider("ARENA_TIMER", "Arena Timer (ticks)", 6000, 100, 60000);

        m_thread = CreateThread(NULL, 0, timerThread, this, 0, NULL);
    }

    ~ArenaTimerMod() {
        m_running = false;
        if (m_thread) {
            WaitForSingleObject(m_thread, 1000);
            CloseHandle(m_thread);
        }
    }
};

extern "C" __declspec(dllexport) HamsterballAPI* CreateModInstance() {
    return new ArenaTimerMod();
}