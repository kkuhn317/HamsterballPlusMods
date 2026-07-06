#include "HamsterballAPI.h"
#include <windows.h>
#include <mmsystem.h>
#include <string>

#pragma comment(lib, "winmm.lib")

typedef DWORD(WINAPI* GetTickCount_t)();
typedef void(__thiscall* DrawFunc)(void*);

GetTickCount_t OriginalGetTickCount = nullptr;
DrawFunc OriginalDraw = nullptr;

DWORD WINAPI MyGetTickCount() {
    return timeGetTime();
}

LARGE_INTEGER timerFreq;
LARGE_INTEGER lastDrawTime;
int targetFps = 120;
LARGE_INTEGER lastFpsTime;
int frameCount = 0;
int currentFps = 0;

void __fastcall CustomDraw(void* pApp, void* edx) {
    LARGE_INTEGER currentTime;
    double targetMs = 1000.0 / (float)targetFps;

    while (true) {
        QueryPerformanceCounter(&currentTime);
        double elapsed = (currentTime.QuadPart - lastDrawTime.QuadPart) * 1000.0 / timerFreq.QuadPart;

        if (elapsed >= targetMs) break;

        if ((targetMs - elapsed) > 2.0) {
            Sleep(1);
        }
    }

    lastDrawTime = currentTime;
    frameCount++;

    double fpsElapsed = (currentTime.QuadPart - lastFpsTime.QuadPart) * 1000.0 / timerFreq.QuadPart;
    if (fpsElapsed >= 1000.0) {
        currentFps = frameCount;
        frameCount = 0;
        lastFpsTime = currentTime;
    }

    // --- The True Omni-Buzzsaw Fix ---
    // todo: this doesnt do anything, please remove and check that nothing broke
    static float visualTickAccumulator = 0.0f;
    float fpsCorrection = 64.0f / (float)targetFps;
    visualTickAccumulator += fpsCorrection;

    if (visualTickAccumulator < 1.0f) {
        // The engine's main loop (App_Run) just added +1 to these visual counters 
        // right before calling this Draw function. 
        // We subtract 1 to hold the animations steady and match 64fps!
        DWORD* pAnimCounter1 = (DWORD*)((DWORD)pApp + 0x18C);
        DWORD* pAnimCounter2 = (DWORD*)((DWORD)pApp + 0x194);

        *pAnimCounter1 -= 1;
        *pAnimCounter2 -= 1;
    }
    else {
        // A full 64fps frame passed! Let the engine's +1 stay.
        visualTickAccumulator -= 1.0f;
    }

    // Now we let the engine draw using our perfectly scaled counters!
    OriginalDraw(pApp);
}

class FPS_Unlocker : public HamsterballAPI {
private:
    IModAPI* api = nullptr;
    CustomText fpsDisplay;
    App* app;
public:
    const char* GetModName() override { return "FPS Unlocker"; }
    const char* GetAuthorName() override { return "BookwormKevin"; }
    const char* GetContributors() override { return "Hamsterbot"; }

    int GetApiVersion() override { return HAMSTERBALL_API_VERSION; }

    void Initialize(IModAPI* modApi) override {
        api = modApi;
        timeBeginPeriod(1);
        modApi->RegisterCustomHook((DWORD)&GetTickCount, (void*)&MyGetTickCount, (void**)&OriginalGetTickCount);

        QueryPerformanceFrequency(&timerFreq);
        QueryPerformanceCounter(&lastDrawTime);
        QueryPerformanceCounter(&lastFpsTime);

        CustomButton showFPSButton("SHOW_FPS", "SHOW FPS");
        showFPSButton.trueText = "ON";
        showFPSButton.falseText = "OFF";
        showFPSButton.defaultState = "OFF";
        api->CreateToggleButton(showFPSButton, this);

        CustomSlider fpsSlider("TARGET_FPS", "TARGET FPS", targetFps);
        fpsSlider.stepSize = 10;
        fpsSlider.lowerBound = 10;
        api->CreateSlider(fpsSlider, this);

        app = api->GetApp();
        *(int*)((DWORD)app + 0x170) = 200;

        DWORD* vtable = *(DWORD**)app;
        DWORD oldProtect;
        VirtualProtect((void*)&vtable[10], sizeof(DWORD), PAGE_EXECUTE_READWRITE, &oldProtect);
        OriginalDraw = (DrawFunc)vtable[10];
        vtable[10] = (DWORD)CustomDraw;
        VirtualProtect((void*)&vtable[10], sizeof(DWORD), oldProtect, &oldProtect);
    }

    void onSliderChange(const char* sliderId, float newValue) override {
        if (strcmp(sliderId, "TARGET_FPS") == 0) targetFps = (int)newValue;
    }

    void onBallUpdate(Ball* ball) override {}

    void onTextRenderLoop() override {
        if (api->GetButtonState("SHOW_FPS")) {
            fpsDisplay.text_color = Color(0.f, 0.f, 0.f);
            fpsDisplay.font = app->fonts.arialNarrow12bold;
            fpsDisplay.enable_shadow = false;

            std::string fpsStr = "FPS: " + std::to_string(currentFps);

            fpsDisplay.x = fpsStr.length() * 4;
            fpsDisplay.y = 0;

            api->DrawCustomText(fpsStr.c_str(), fpsDisplay);
        }
    }
};

extern "C" __declspec(dllexport) HamsterballAPI* CreateModInstance() {
    return new FPS_Unlocker();
}