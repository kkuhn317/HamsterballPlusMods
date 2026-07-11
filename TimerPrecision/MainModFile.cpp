#define _CRT_SECURE_NO_WARNINGS
#include "HamsterballAPI.h"
#include <string.h>

// ============================================================================
// Timer Precision Mod — toggles ALL timer displays between 1 and 2 decimal places
//
// Game timer: 100 ticks = 1 second
// 1-digit:  integer=timer/100, decimal=(timer/10)%10 → "12.3"
// 2-digit:  integer=timer/100, decimal=timer%100     → "12.34"
//
// Patches 14 sites across 6 functions. Each site has two patches:
// 1. Format string PUSH: ".%.1d" (0x4D03F0) ↔ ".%.2d" (DLL-local string)
// 2. Computation (27 bytes): (timer/10)%10 ↔ timer%100
//
// Three register variants: ECX=0xC1, EDI=0xC7, EBP=0xC5
// ============================================================================

struct PatchSite {
    DWORD computeAddr;
    DWORD fmtPushAddr;
    unsigned char movReg; // 0xC1=ECX, 0xC7=EDI, 0xC5=EBP
    const char* desc;
};

class TimerPrecision : public HamsterballAPI {
private:
    IModAPI* api = nullptr;
    bool g_initialized = false;

    static const char* s_decimalFmt2;
    static constexpr int NUM_PATCHES = 14;
    static const PatchSite s_patches[NUM_PATCHES];

    static void applyMode(IModAPI* api, bool twoDigits) {
        DWORD fmtAddr = twoDigits ? (DWORD)s_decimalFmt2 : 0x004D03F0;

        unsigned char fmtPush[5] = { 0x68, 0, 0, 0, 0 };
        memcpy(&fmtPush[1], &fmtAddr, 4);

        for (int i = 0; i < NUM_PATCHES; i++) {
            const PatchSite& p = s_patches[i];

            // Patch 1: Format string PUSH (5 bytes)
            api->PatchMemory(p.fmtPushAddr, (const char*)fmtPush, 5);

            // Patch 2: Computation (27 bytes)
            unsigned char compute[27];
            memset(compute, 0x90, 27); // fill with NOPs

            compute[0] = 0x8B; compute[1] = p.movReg; // MOV EAX,<reg>
            compute[2] = 0x99;                         // CDQ

            if (twoDigits) {
                // timer % 100 via unsigned DIV
                // Result in EDX = timer % 100 (2-digit decimal display)
                compute[3] = 0xB9; compute[4] = 0x64; compute[5] = 0x00;
                compute[6] = 0x00; compute[7] = 0x00;   // MOV ECX,100
                compute[8] = 0xF7; compute[9] = 0xF1;   // DIV ECX → EDX=timer%100
                // remaining 17 bytes already NOP
            }
            else {
                // (timer/10)%10 via two IDIVs
                // Result in EDX = (timer/10)%10 (1-digit decimal display)
                compute[3] = 0xB9; compute[4] = 0x0A; compute[5] = 0x00;
                compute[6] = 0x00; compute[7] = 0x00;   // MOV ECX,10
                compute[8] = 0xF7; compute[9] = 0xF9;    // IDIV ECX → EAX=timer/10
                compute[10] = 0x99;                        // CDQ (sign-extend EAX)
                compute[11] = 0xF7; compute[12] = 0xF9;   // IDIV ECX → EDX=(timer/10)%10
                // remaining 14 bytes already NOP
            }
            api->PatchMemory(p.computeAddr, (const char*)compute, 27);
        }
    }

public:
    const char* GetModName() override { return "Timer Precision"; }
    const char* GetAuthorName() override { return "BookwormKevin"; }
    int GetApiVersion() override { return HAMSTERBALL_API_VERSION; }

    void Initialize(IModAPI* modApi) override {
        api = modApi;
        CustomButton btn;
        btn.id = "timer_precision";
        btn.displayText = "Timer Decimals";
        btn.trueText = "2";
        btn.falseText = "1";
        btn.defaultState = true;
        api->CreateToggleButton(btn, this);
    }

    void onGameUpdate() override {
        if (g_initialized) return;
        g_initialized = true;
        bool twoDigits = api->GetButtonState("timer_precision");
        applyMode(api, twoDigits);
    }

    void onButtonToggle(const char* id, bool state) override {
        if (strcmp(id, "timer_precision") != 0) return;
        applyMode(api, state);
    }
};

const char* TimerPrecision::s_decimalFmt2 = ".%.2d";

const PatchSite TimerPrecision::s_patches[] = {
    {0x421B8C, 0x421BB3, 0xC1, "arena timer"},
    {0x41BE1C, 0x41BE47, 0xC1, "race HUD timer"},
    {0x41C229, 0x41C250, 0xC7, "split-screen timer #1"},
    {0x41C4C5, 0x41C4EC, 0xC7, "split-screen timer #2"},
    {0x44CF86, 0x44CFB7, 0xC1, "results: time remaining"},
    {0x44D18A, 0x44D1B8, 0xC1, "results: par time"},
    {0x44E25F, 0x44E290, 0xC1, "tt results: race time"},
    {0x44E448, 0x44E479, 0xC1, "tt results: best time"},
    {0x44E63C, 0x44E66D, 0xC1, "tt results: weasel time"},
    {0x44EB19, 0x44EB4A, 0xC1, "tt results: bronze time"},
    {0x44ECFF, 0x44ED30, 0xC1, "tt results: silver time"},
    {0x44EEE5, 0x44EF16, 0xC1, "tt results: gold time"},
    {0x451157, 0x451185, 0xC1, "tournament menu #1"},
    {0x451935, 0x451963, 0xC5, "tournament menu #2"},
};

extern "C" __declspec(dllexport) HamsterballAPI* CreateModInstance() {
    return new TimerPrecision();
}