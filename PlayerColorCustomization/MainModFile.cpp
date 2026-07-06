#include "HamsterballAPI.h"
#include <windows.h>

static constexpr DWORD BOARD_COLOR_BASE = 0x3AB0;
static constexpr DWORD BOARD_COLOR_STRIDE = 0x14;
static constexpr DWORD APP_PROFILE_OFFSET = 0x220;
static constexpr DWORD PROFILE_BOARD_OFFSET = 0x0C;
static constexpr DWORD BOARD_VTABLE_MIN = 0x4D0000;
static constexpr DWORD BOARD_VTABLE_MAX = 0x4D2000;
static constexpr DWORD GLOBAL_APP_PTR = 0x5341E0;

static constexpr DWORD AB_P1_R = 0x421BFD, AB_P1_G = 0x421BF8, AB_P1_B = 0x421BF3, AB_P1_A = 0x421BEE;
static constexpr DWORD AB_P2_G = 0x421CBB, AB_P2_B = 0x421CB6, AB_P2_A = 0x421CB1;
static constexpr DWORD AB_P3_R = 0x421D85, AB_P3_G = 0x421D80, AB_P3_B = 0x421D7B, AB_P3_A = 0x421D76;
static constexpr DWORD AB_P4_R = 0x421E4A, AB_P4_A = 0x421E3F;

static constexpr DWORD ALS_P1_R_2P = 0x433116, ALS_P1_G_2P = 0x433111, ALS_P1_B_2P = 0x43310C, ALS_P1_A_2P = 0x433107;
static constexpr DWORD ALS_P2_G = 0x433025, ALS_P2_B = 0x433020;
static constexpr DWORD ALS_P3_R = 0x433063, ALS_P3_G = 0x43305E, ALS_P3_B = 0x433059;
static constexpr DWORD ALS_P4_R = 0x43309C;

static constexpr DWORD DM_P1_R = 0x431B3C, DM_P1_G = 0x431B37, DM_P1_B = 0x431B32, DM_P1_A = 0x431B2D;
static constexpr DWORD DM_P2_G = 0x431B6E, DM_P2_B = 0x431B69, DM_P2_A = 0x431B64;

static constexpr DWORD RRM_P1_R = 0x44F0F3, RRM_P1_G = 0x44F0EE, RRM_P1_B = 0x44F0E9, RRM_P1_A = 0x44F0E4;
static constexpr DWORD RRM_P2_G = 0x44F11C, RRM_P2_B = 0x44F117, RRM_P2_A = 0x44F112;
static constexpr DWORD RRM_P3_R = 0x44F169, RRM_P3_G = 0x44F164, RRM_P3_B = 0x44F15F, RRM_P3_A = 0x44F15A;
static constexpr DWORD RRM_P4_R = 0x44F1B1, RRM_P4_G = 0x44F1AC, RRM_P4_A = 0x44F1A5;

static float g_p2_red = 0.0f;
static float g_p4_blue = 0.0f;
static float g_p4_green = 1.0f;

enum SavedType { SAVED_CALL, SAVED_LEA, SAVED_PUSH_GLOBAL };
struct CodeCave { void* caveAddr; };
static CodeCave g_caves[7];

static void installCave(int idx, DWORD patchSite, float* globalFloat,
    SavedType savedType, DWORD callTarget, BYTE leaOffset,
    float* secondGlobal, DWORD returnAddr) {
    BYTE caveCode[32];
    int p = 0;

    caveCode[p++] = 0xFF;
    caveCode[p++] = 0x35;
    *(DWORD*)(caveCode + p) = (DWORD)globalFloat;
    p += 4;

    int savedLen;
    if (savedType == SAVED_CALL) {
        savedLen = 5;
        caveCode[p++] = 0xE8;
        *(DWORD*)(caveCode + p) = 0;
        p += 4;
    }
    else if (savedType == SAVED_LEA) {
        savedLen = 4;
        caveCode[p++] = 0x8D;
        caveCode[p++] = 0x4C;
        caveCode[p++] = 0x24;
        caveCode[p++] = leaOffset;
    }
    else {
        savedLen = 5;
        caveCode[p++] = 0xFF;
        caveCode[p++] = 0x35;
        *(DWORD*)(caveCode + p) = (DWORD)secondGlobal;
        p += 4;
    }

    int jmpOffset = p;
    caveCode[p++] = 0xE9;
    *(DWORD*)(caveCode + p) = 0;
    p += 4;
    int totalLen = p;

    void* cave = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!cave) return;
    g_caves[idx].caveAddr = cave;
    memcpy(cave, caveCode, totalLen);

    DWORD caveBase = (DWORD)cave;

    if (savedType == SAVED_CALL) {
        DWORD callAddr = caveBase + 6;
        *(DWORD*)(caveBase + 7) = callTarget - (callAddr + 5);
    }

    DWORD jmpSrc = caveBase + jmpOffset;
    *(DWORD*)(caveBase + jmpOffset + 1) = returnAddr - (jmpSrc + 5);

    int patchLen = (savedLen == 4) ? 5 : 6;
    DWORD oldProtect;
    VirtualProtect((void*)patchSite, patchLen, PAGE_EXECUTE_READWRITE, &oldProtect);
    *(BYTE*)(patchSite) = 0xE9;
    *(DWORD*)(patchSite + 1) = caveBase - (patchSite + 5);
    if (patchLen == 6) *(BYTE*)(patchSite + 5) = 0x90;
    VirtualProtect((void*)patchSite, patchLen, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), (void*)patchSite, patchLen);
}

class BallTintMod : public HamsterballAPI {
private:
    IModAPI* api = nullptr;
    HANDLE m_thread = NULL;
    volatile bool m_running = true;
    bool m_cavesInstalled = false;

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

    static void applyBoardColor(DWORD board, int playerIndex, float r, float g, float b) {
        DWORD addr = board + BOARD_COLOR_BASE + (playerIndex * BOARD_COLOR_STRIDE);
        if (IsBadWritePtr((void*)addr, 16)) return;
        *(float*)(addr + 0x00) = r;
        *(float*)(addr + 0x04) = g;
        *(float*)(addr + 0x08) = b;
        *(float*)(addr + 0x0C) = 1.0f;
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
        if (!validateBoard(board)) return 0;
        return board;
    }

    void patchFloat(DWORD addr, float value) {
        api->PatchMemory(addr, (const char*)&value, sizeof(float));
    }

    void installCaves() {
        if (m_cavesInstalled) return;

        installCave(0, 0x421CBF, &g_p2_red, SAVED_CALL, 0x453150, 0, nullptr, 0x421CC5);
        installCave(1, 0x421E43, &g_p4_blue, SAVED_PUSH_GLOBAL, 0, 0, &g_p4_green, 0x421E49);
        installCave(2, 0x433029, &g_p2_red, SAVED_LEA, 0, 0x28, nullptr, 0x43302E);
        installCave(3, 0x433095, &g_p4_blue, SAVED_PUSH_GLOBAL, 0, 0, &g_p4_green, 0x43309B);
        installCave(4, 0x431B72, &g_p2_red, SAVED_CALL, 0x453150, 0, nullptr, 0x431B78);
        installCave(5, 0x44F120, &g_p2_red, SAVED_LEA, 0, 0x30, nullptr, 0x44F126);
        installCave(6, 0x44F1A9, &g_p4_blue, SAVED_PUSH_GLOBAL, 0, 0, &g_p4_green, 0x44F1B0);

        m_cavesInstalled = true;
    }

    void patchScoreballColors() {
        float p1r = api->GetSliderState("TINT_P1_R");
        float p1g = api->GetSliderState("TINT_P1_G");
        float p1b = api->GetSliderState("TINT_P1_B");
        float p2r = api->GetSliderState("TINT_P2_R");
        float p2g = api->GetSliderState("TINT_P2_G");
        float p2b = api->GetSliderState("TINT_P2_B");
        float p3r = api->GetSliderState("TINT_P3_R");
        float p3g = api->GetSliderState("TINT_P3_G");
        float p3b = api->GetSliderState("TINT_P3_B");
        float p4r = api->GetSliderState("TINT_P4_R");
        float p4g = api->GetSliderState("TINT_P4_G");
        float p4b = api->GetSliderState("TINT_P4_B");

        g_p2_red = p2r;
        g_p4_blue = p4b;
        g_p4_green = p4g;

        patchFloat(AB_P1_R, p1r); patchFloat(AB_P1_G, p1g); patchFloat(AB_P1_B, p1b); patchFloat(AB_P1_A, 1.0f);
        patchFloat(AB_P2_G, p2g); patchFloat(AB_P2_B, p2b); patchFloat(AB_P2_A, 1.0f);
        patchFloat(AB_P3_R, p3r); patchFloat(AB_P3_G, p3g); patchFloat(AB_P3_B, p3b); patchFloat(AB_P3_A, 1.0f);
        patchFloat(AB_P4_R, p4r); patchFloat(AB_P4_A, 1.0f);

        patchFloat(ALS_P1_R_2P, p1r); patchFloat(ALS_P1_G_2P, p1g); patchFloat(ALS_P1_B_2P, p1b); patchFloat(ALS_P1_A_2P, 1.0f);
        patchFloat(ALS_P2_G, p2g); patchFloat(ALS_P2_B, p2b);
        patchFloat(ALS_P3_R, p3r); patchFloat(ALS_P3_G, p3g); patchFloat(ALS_P3_B, p3b);
        patchFloat(ALS_P4_R, p4r);

        patchFloat(DM_P1_R, p1r); patchFloat(DM_P1_G, p1g); patchFloat(DM_P1_B, p1b); patchFloat(DM_P1_A, 1.0f);
        patchFloat(DM_P2_G, p2g); patchFloat(DM_P2_B, p2b); patchFloat(DM_P2_A, 1.0f);

        patchFloat(RRM_P1_R, p1r); patchFloat(RRM_P1_G, p1g); patchFloat(RRM_P1_B, p1b); patchFloat(RRM_P1_A, 1.0f);
        patchFloat(RRM_P2_G, p2g); patchFloat(RRM_P2_B, p2b); patchFloat(RRM_P2_A, 1.0f);
        patchFloat(RRM_P3_R, p3r); patchFloat(RRM_P3_G, p3g); patchFloat(RRM_P3_B, p3b); patchFloat(RRM_P3_A, 1.0f);
        patchFloat(RRM_P4_R, p4r); patchFloat(RRM_P4_A, 1.0f);
    }

    static DWORD WINAPI tintThread(LPVOID param) {
        BallTintMod* self = (BallTintMod*)param;
        IModAPI* api = self->api;

        Sleep(3000);
        self->installCaves();

        while (self->m_running) {
            Sleep(16);
            self->patchScoreballColors();

            DWORD board = findBoard();
            if (board) {
                applyBoardColor(board, 0,
                    api->GetSliderState("TINT_P1_R"),
                    api->GetSliderState("TINT_P1_G"),
                    api->GetSliderState("TINT_P1_B"));
                applyBoardColor(board, 1,
                    api->GetSliderState("TINT_P2_R"),
                    api->GetSliderState("TINT_P2_G"),
                    api->GetSliderState("TINT_P2_B"));
                applyBoardColor(board, 2,
                    api->GetSliderState("TINT_P3_R"),
                    api->GetSliderState("TINT_P3_G"),
                    api->GetSliderState("TINT_P3_B"));
                applyBoardColor(board, 3,
                    api->GetSliderState("TINT_P4_R"),
                    api->GetSliderState("TINT_P4_G"),
                    api->GetSliderState("TINT_P4_B"));
            }
        }
        return 0;
    }

public:
    const char* GetModName() override { return "Ball Tint"; }
    const char* GetAuthorName() override { return "BookwormKevin"; }
    const char* GetContributors() override { return "Hamsterbot"; }
    int GetApiVersion() override { return HAMSTERBALL_API_VERSION; }

    void Initialize(IModAPI* modApi) override {
        api = modApi;

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

        m_thread = CreateThread(NULL, 0, tintThread, this, 0, NULL);
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