#define _CRT_SECURE_NO_WARNINGS
#include "HamsterballAPI.h"
#include <cmath>

static bool g_hideUI = false;
static bool g_hideBall = false;
static bool g_disableFog = false;
static bool g_fogSaved = false;
static float g_origFogStart = 0.0f;
static float g_origFogEnd = 0.0f;
static float g_origFarPlane = 0.0f;

// Font_DrawGlyph: RET 0x20 = 8 stack params → 10 total (ecx + edx + 8)
typedef void(__fastcall* FontDrawGlyph_t)(void*, void*, const char*, int, int,
    void*, void*, void*, void*, void*);
static FontDrawGlyph_t orig_FontDrawGlyph = nullptr;

// Sprite_DrawRotatedQuad: RET 0x24 = 9 stack params → 11 total
typedef void(__fastcall* SpriteDrawRotatedQuad_t)(void*, void*, void*, void*, void*, void*, void*, void*, void*, void*, void*);
static SpriteDrawRotatedQuad_t orig_SpriteDrawRotatedQuad = nullptr;

// Graphics_DrawScreenRect: RET 0x24 = 9 stack params → 11 total
typedef void(__fastcall* GraphicsDrawScreenRect_t)(void*, void*, void*, void*, void*, void*, void*, void*, void*, void*, void*);
static GraphicsDrawScreenRect_t orig_GraphicsDrawScreenRect = nullptr;

// Sprite_DrawExtended (FUN_0045d450): RET 0x3C = 15 stack params → 17 total
// This is the function that draws timer blots, ready/set/go images, etc.
// It calls DrawPrimitiveUP directly — NOT through Sprite_DrawRect/RenderQuad.
typedef void(__fastcall* SpriteDrawExtended_t)(void*, void*, void*, void*, void*, void*, void*, void*, void*, void*, void*, void*, void*, void*, void*, void*, void*);
static SpriteDrawExtended_t orig_SpriteDrawExtended = nullptr;

static void __fastcall hook_FontDrawGlyph(void* thisPtr, void* edx, const char* text, int x, int y,
    void* p4, void* p5, void* p6, void* p7, void* p8) {
    if (g_hideUI) return;
    orig_FontDrawGlyph(thisPtr, edx, text, x, y, p4, p5, p6, p7, p8);
}

static void __fastcall hook_SpriteDrawRotatedQuad(void* thisPtr, void* edx,
    void* p1, void* p2, void* p3, void* p4, void* p5, void* p6, void* p7, void* p8, void* p9) {
    if (g_hideUI) return;
    orig_SpriteDrawRotatedQuad(thisPtr, edx, p1, p2, p3, p4, p5, p6, p7, p8, p9);
}

static void __fastcall hook_GraphicsDrawScreenRect(void* thisPtr, void* edx,
    void* p1, void* p2, void* p3, void* p4, void* p5, void* p6, void* p7, void* p8, void* p9) {
    if (g_hideUI) return;
    orig_GraphicsDrawScreenRect(thisPtr, edx, p1, p2, p3, p4, p5, p6, p7, p8, p9);
}

static void __fastcall hook_SpriteDrawExtended(void* thisPtr, void* edx,
    void* p1, void* p2, void* p3, void* p4, void* p5, void* p6, void* p7,
    void* p8, void* p9, void* p10, void* p11, void* p12, void* p13, void* p14, void* p15) {
    if (g_hideUI) return;
    orig_SpriteDrawExtended(thisPtr, edx, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15);
}

class FreeCamMod : public HamsterballAPI {
private:
    IModAPI* api = nullptr;
    bool active = false;
    bool initialized = false;

    float eyeX = 0, eyeY = 0, eyeZ = 0;
    float yaw = 0, pitch = 0;

    static constexpr float MOVE_SPEED = 800.0f;
    static constexpr float LOOK_SPEED = 0.025f;
    static constexpr float SHIFT_MULT = 3.0f;
    static constexpr float MAX_PITCH = 1.55f;
    static constexpr float DT = 0.016f;

    void initCamera() {
        Ball* ball = api->GetPlayer();
        if (!ball || IsBadReadPtr(ball, sizeof(Ball))) {
            eyeX = 0; eyeY = 500; eyeZ = 0;
            yaw = 0; pitch = -0.3f;
            initialized = true;
            return;
        }
        eyeX = ball->pos_x;
        eyeY = ball->pos_y + 200.0f;
        eyeZ = ball->pos_z - 400.0f;
        float dx = ball->pos_x - eyeX;
        float dy = ball->pos_y - eyeY;
        float dz = ball->pos_z - eyeZ;
        float horiz = sqrtf(dx * dx + dz * dz);
        yaw = atan2f(dx, dz);
        pitch = atan2f(dy, horiz);
        initialized = true;
    }

    void forwardVec(float& fx, float& fy, float& fz) {
        float cp = cosf(pitch);
        fx = sinf(yaw) * cp;
        fy = sinf(pitch);
        fz = cosf(yaw) * cp;
    }

    void rightVec(float& rx, float& ry, float& rz) {
        float fx, fy, fz;
        forwardVec(fx, fy, fz);
        rx = fz;
        ry = 0.0f;
        rz = -fx;
    }

public:
    const char* GetModName() override { return "FreeCam"; }
    const char* GetAuthorName() override { return "BookwormKevin"; }
    const char* GetContributors() override { return "Hamsterbot"; }
    int GetApiVersion() override { return HAMSTERBALL_API_VERSION; }

    void Initialize(IModAPI* modApi) override {
        api = modApi;
        api->RegisterCustomControl("FREECAM_TOGGLE", CustomControl(DIK_F7));
        api->RegisterCustomControl("FREECAM_HIDEUI", CustomControl(DIK_F8));
        api->RegisterCustomControl("FREECAM_HIDEBALL", CustomControl(DIK_F9));
        api->RegisterCustomControl("FREECAM_FOG", CustomControl(DIK_F10));
        api->RegisterCustomHook(0x457440, (void*)hook_FontDrawGlyph, (void**)&orig_FontDrawGlyph);
        api->RegisterCustomHook(0x45DAB0, (void*)hook_SpriteDrawRotatedQuad, (void**)&orig_SpriteDrawRotatedQuad);
        api->RegisterCustomHook(0x455D60, (void*)hook_GraphicsDrawScreenRect, (void**)&orig_GraphicsDrawScreenRect);
        api->RegisterCustomHook(0x45D450, (void*)hook_SpriteDrawExtended, (void**)&orig_SpriteDrawExtended);
        printf("[FreeCam] Ready. F7=toggle cam, F8=toggle UI\n");
    }

    void onGameUpdate() override {
        if (!api) return;

        if (api->WasControlPressed("FREECAM_TOGGLE")) {
            Scene* scene = api->GetScene();
            if (!scene) {
                printf("[FreeCam] Can't activate in menu\n");
                return;
            }
            active = !active;
            if (active) {
                initCamera();
                printf("[FreeCam] ON\n");
            }
            else {
                initialized = false;
                printf("[FreeCam] OFF\n");
            }
        }

        if (api->WasControlPressed("FREECAM_HIDEUI")) {
            g_hideUI = !g_hideUI;
            printf("[FreeCam] UI: %s\n", g_hideUI ? "HIDDEN" : "VISIBLE");
        }

        if (api->WasControlPressed("FREECAM_HIDEBALL")) {
            g_hideBall = !g_hideBall;
            if (!g_hideBall) {
                // Toggling back to visible — force alpha to 1.0 once
                // so the ball isn't stuck invisible
                Ball* ball = api->GetPlayer();
                if (ball && !IsBadReadPtr(ball, sizeof(Ball))) {
                    *(float*)((uint8_t*)ball + 0x2FC) = 1.0f;
                }
            }
            printf("[FreeCam] Ball: %s\n", g_hideBall ? "HIDDEN" : "VISIBLE");
        }

        // When hidden, force alpha to 0.0 every frame — UNLESS the ball
        // is in respawn fade-in (ball+0x2F9 set). During respawn, let
        // alpha go to 1.0 so the respawn check passes and movement unlocks.
        // The ball will be briefly visible during respawn fade-in, then
        // re-hide once respawn completes.
        if (g_hideBall) {
            Ball* ball = api->GetPlayer();
            if (ball && !IsBadReadPtr(ball, sizeof(Ball))) {
                bool respawning = *((uint8_t*)ball + 0x2F9) != 0;
                if (!respawning) {
                    *(float*)((uint8_t*)ball + 0x2FC) = 0.0f;
                }
            }
        }

        if (api->WasControlPressed("FREECAM_FOG")) {
            g_disableFog = !g_disableFog;
            printf("[FreeCam] Fog: %s\n", g_disableFog ? "DISABLED" : "ENABLED");
        }

        // Fog + draw distance: push FOGSTART/FOGEND to extreme distances,
        // and rebuild projection matrix with large far clip plane.
        // gfx = App+0x174, D3D device = gfx+0x154, SetRenderState = vtable[50].
        // FOGSTART = 36, FOGEND = 37 (values are float bits).
        // Graphics_SetProjection at RVA 0x54AB0 = __thiscall(gfx, near, far).
        // Original near=20.0, far=stored at gfx+0x794. We push far to 100000.
        // On re-enable: restore original FOGSTART/FOGEND from gfx struct and
        // rebuild projection with original far plane (gfx+0x794 was overwritten
        // by our call, but gfx+0x790=near and gfx+0x184=screen dist still exist).
        // Simplest: read original far from gfx+0x794 BEFORE first override.
        if (g_disableFog) {
            App* app = api->GetApp();
            if (app && !IsBadReadPtr(app, sizeof(App))) {
                void* gfx = app->graphics;
                if (gfx && !IsBadReadPtr(gfx, 0x800)) {
                    // Save original fog/projection values on first use
                    if (!g_fogSaved) {
                        g_origFogStart = *(float*)((uint8_t*)gfx + 0x73C);
                        g_origFogEnd = *(float*)((uint8_t*)gfx + 0x740);
                        g_origFarPlane = *(float*)((uint8_t*)gfx + 0x794);
                        g_fogSaved = true;
                    }
                    // Push fog start/end to extreme distances via D3D SetRenderState
                    void* device = *(void**)((uint8_t*)gfx + 0x154);
                    if (device && !IsBadReadPtr(device, 4)) {
                        void** vtable = *(void***)device;
                        if (vtable && !IsBadReadPtr(vtable, 0xCC)) {
                            typedef long(__stdcall* D3DSetRenderState_t)(void*, DWORD, DWORD);
                            D3DSetRenderState_t srs = (D3DSetRenderState_t)vtable[50];
                            float fogStart = 99999.0f;
                            float fogEnd = 100000.0f;
                            srs(device, 36, *(DWORD*)&fogStart);
                            srs(device, 37, *(DWORD*)&fogEnd);
                        }
                    }
                    // Rebuild projection matrix with large far clip plane
                    DWORD projAddr = (DWORD)GetModuleHandle(NULL) + 0x54AB0;
                    typedef void(__thiscall* SetProj_t)(void*, float, float);
                    SetProj_t setProj = (SetProj_t)projAddr;
                    setProj((void*)gfx, 20.0f, 100000.0f);
                }
            }
        }
        else if (g_fogSaved) {
            // Restore original fog and projection
            App* app = api->GetApp();
            if (app && !IsBadReadPtr(app, sizeof(App))) {
                void* gfx = app->graphics;
                if (gfx && !IsBadReadPtr(gfx, 0x800)) {
                    void* device = *(void**)((uint8_t*)gfx + 0x154);
                    if (device && !IsBadReadPtr(device, 4)) {
                        void** vtable = *(void***)device;
                        if (vtable && !IsBadReadPtr(vtable, 0xCC)) {
                            typedef long(__stdcall* D3DSetRenderState_t)(void*, DWORD, DWORD);
                            D3DSetRenderState_t srs = (D3DSetRenderState_t)vtable[50];
                            srs(device, 36, *(DWORD*)&g_origFogStart);
                            srs(device, 37, *(DWORD*)&g_origFogEnd);
                        }
                    }
                    DWORD projAddr = (DWORD)GetModuleHandle(NULL) + 0x54AB0;
                    typedef void(__thiscall* SetProj_t)(void*, float, float);
                    SetProj_t setProj = (SetProj_t)projAddr;
                    setProj((void*)gfx, 20.0f, g_origFarPlane);
                }
            }
            g_fogSaved = false;
        }

        if (!active || !initialized) return;

        Scene* scene = api->GetScene();
        if (!scene) {
            active = false;
            initialized = false;
            printf("[FreeCam] Deactivated (left level)\n");
            return;
        }

        float speed = MOVE_SPEED * DT;
        if (api->IsKeyDown(DIK_LSHIFT)) speed *= SHIFT_MULT;

        float fx, fy, fz, rx, ry, rz;
        forwardVec(fx, fy, fz);
        rightVec(rx, ry, rz);

        if (api->IsKeyDown(DIK_W)) { eyeX += fx * speed; eyeY += fy * speed; eyeZ += fz * speed; }
        if (api->IsKeyDown(DIK_S)) { eyeX -= fx * speed; eyeY -= fy * speed; eyeZ -= fz * speed; }
        if (api->IsKeyDown(DIK_D)) { eyeX += rx * speed; eyeY += ry * speed; eyeZ += rz * speed; }
        if (api->IsKeyDown(DIK_A)) { eyeX -= rx * speed; eyeY -= ry * speed; eyeZ -= rz * speed; }
        if (api->IsKeyDown(DIK_E)) { eyeY += speed; }
        if (api->IsKeyDown(DIK_Q)) { eyeY -= speed; }

        if (api->IsKeyDown(DIK_UP))    pitch += LOOK_SPEED;
        if (api->IsKeyDown(DIK_DOWN))  pitch -= LOOK_SPEED;
        if (api->IsKeyDown(DIK_LEFT))  yaw -= LOOK_SPEED;
        if (api->IsKeyDown(DIK_RIGHT)) yaw += LOOK_SPEED;

        if (pitch > MAX_PITCH) pitch = MAX_PITCH;
        if (pitch < -MAX_PITCH) pitch = -MAX_PITCH;
    }

    void onRenderApply(void* this_ptr, float* viewMatrix) override {
        if (!active || !initialized) return;

        float fx, fy, fz;
        forwardVec(fx, fy, fz);

        Vec3 eye = { eyeX, eyeY, eyeZ };
        Vec3 target = { eyeX + fx, eyeY + fy, eyeZ + fz };
        Vec3 up = { 0.0f, 1.0f, 0.0f };
        BuildCustomViewMatrix(viewMatrix, eye, target, up);
    }

    void onTextRenderLoop() override {
        if (!active || !api || g_hideUI) return;
        App* app = api->GetApp();
        if (!app || IsBadReadPtr(app, sizeof(App))) return;

        CustomText ct;
        ct.font = app->fonts.showcardGothic14;
        ct.x = 90;
        ct.y = 10;
        ct.text_color = Color(0.0f, 1.0f, 0.0f, 1.0f);
        ct.enable_shadow = true;
        api->DrawCustomText("FREECAM", ct);
    }

    void onLevelStart() override {
        if (active) {
            initCamera();
            printf("[FreeCam] Re-initialized for new level\n");
        }
    }

    void onSceneEnd() override {
        if (active) {
            active = false;
            initialized = false;
            printf("[FreeCam] Deactivated (scene end)\n");
        }
    }
};

extern "C" __declspec(dllexport) HamsterballAPI* CreateModInstance() {
    return new FreeCamMod();
}