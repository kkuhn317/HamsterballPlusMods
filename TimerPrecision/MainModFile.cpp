#define _CRT_SECURE_NO_WARNINGS
#include "HamsterballAPI.h"
#include <string.h>

// ============================================================================
// Timer Precision Mod — increases ALL timer displays from 1 to 2 decimal places
//
// Game timer: 100 ticks = 1 second
// Original:  integer=timer/100, decimal=(timer/10)%10  →  "12.3"
// Patched:   integer=timer/100, decimal=timer%100        →  "12.34"
//
// Patches 14 sites across 6 functions:
//   ArenaBoard_Render (0x421910)  — arena timer
//   FUN_0041b710 (0x41b710)      — race HUD timer
//   FUN_0041bfd0 (0x41bfd0)       — split-screen timer (2 sites)
//   FUN_0044cd10 (0x44cd10)       — time trial results (2 sites)
//   FUN_0044df70 (0x44df70)       — race results (6 sites)
//   TourneyMenu_Render            — tournament menu (2 sites)
//
// Each site has two patches:
//   1. Format string PUSH: change ".%.1d" → ".%.2d" (custom string in DLL)
//   2. Computation (27 bytes): change (timer/10)%10 → timer%100
//
// Three register variants:
//   ECX → MOV EAX,ECX (8B C1) — 11 sites
//   EDI → MOV EAX,EDI (8B C7) — 2 sites
//   EBP → MOV EAX,EBP (8B C5) — 1 site
// ============================================================================

struct PatchSite {
    DWORD computeAddr;
    DWORD fmtPushAddr;
    unsigned char movReg;  // 0xC1=ECX, 0xC7=EDI, 0xC5=EBP
    const char* desc;
};

class TimerPrecision : public HamsterballAPI {
private:
    IModAPI* api = nullptr;

    // Custom format string in DLL memory (referenced by patched PUSH instructions)
    static const char* s_decimalFmt2;

    static constexpr int NUM_PATCHES = 14;
    static const PatchSite s_patches[NUM_PATCHES];

    static void applyPatches(IModAPI* api) {
        DWORD fmtAddr = (DWORD)s_decimalFmt2;

        // Build the 5-byte PUSH instruction for our custom format string
        unsigned char fmtPush[5] = { 0x68, 0, 0, 0, 0 };
        memcpy(&fmtPush[1], &fmtAddr, 4);

        for (int i = 0; i < NUM_PATCHES; i++) {
            const PatchSite& p = s_patches[i];

            // Patch 1: Format string PUSH (5 bytes)
            // Original: PUSH 0x4D03F0 (".%.1d")
            // Patched:  PUSH <s_decimalFmt2> (".%.2d")
            api->PatchMemory(p.fmtPushAddr, (const char*)fmtPush, 5);

            // Patch 2: Computation (27 bytes)
            // Original: (timer/10)%10 via magic multiply 0x66666667 + IDIV 10
            // Patched:  timer%100 via direct IDIV 100
            unsigned char compute[27] = {
                0x8B, p.movReg,                       // MOV EAX,<timer_reg>
                0x99,                                  // CDQ
                0xB9, 0x64, 0x00, 0x00, 0x00,          // MOV ECX,100
                0xF7, 0xF1,                            // DIV ECX (EDX=timer%100)
                0x90, 0x90, 0x90, 0x90, 0x90, 0x90,    // NOP *17
                0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
                0x90, 0x90, 0x90, 0x90, 0x90
            };
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
        btn.displayText = "Timer Precision (2 decimals)";
        btn.defaultState = true;
        api->CreateToggleButton(btn, this);
        applyPatches(api);
    }

    void onButtonToggle(const char* id, bool state) override {
        // Patches applied at init; toggle is visual-only for now.
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