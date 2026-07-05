#define _CRT_SECURE_NO_WARNINGS
#include "HamsterballAPI.h"
#include <cmath>

static bool g_hideUI = false;

// Font_DrawGlyph: RET 0x20 = 8 stack params → 10 total (ecx + edx + 8)
typedef void(__fastcall* FontDrawGlyph_t)(void*, void*, const char*, int, int,
    void*, void*, void*, void*, void*);
static FontDrawGlyph_t orig_FontDrawGlyph = nullptr;

// Sprite_DrawRect: RET 0x1c = 7 stack params → 9 total
typedef void(__fastcall* SpriteDrawRect_t)(void*, void*, void*, void*, void*, void*, void*, void*, void*);
static SpriteDrawRect_t orig_SpriteDrawRect = nullptr;

// Sprite_RenderQuad: RET 0x14 = 5 stack params → 7 total
typedef void(__fastcall* SpriteRenderQuad_t)(void*, void*, void*, void*, void*, void*, void*);
static SpriteRenderQuad_t orig_SpriteRenderQuad = nullptr;

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

static void __fastcall hook_SpriteDrawRect(void* thisPtr, void* edx,
    void* p1, void* p2, void* p3, void* p4, void* p5, void* p6, void* p7) {
    if (g_hideUI) return;
    orig_SpriteDrawRect(thisPtr, edx, p1, p2, p3, p4, p5, p6, p7);
}

static void __fastcall hook_SpriteRenderQuad(void* thisPtr, void* edx,
    void* p1, void* p2, void* p3, void* p4, void* p5) {
    if (g_hideUI) return;
    orig_SpriteRenderQuad(thisPtr, edx, p1, p2, p3, p4, p5);
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
    const char* GetAuthorName() override { return "umans"; }
    int GetApiVersion() override { return HAMSTERBALL_API_VERSION; }

    void Initialize(IModAPI* modApi) override {
        api = modApi;
        api->RegisterCustomControl("FREECAM_TOGGLE", CustomControl(DIK_F7));
        api->RegisterCustomControl("FREECAM_HIDEUI", CustomControl(DIK_F8));
        api->RegisterCustomHook(0x457440, (void*)hook_FontDrawGlyph, (void**)&orig_FontDrawGlyph);
        api->RegisterCustomHook(0x45D300, (void*)hook_SpriteDrawRect, (void**)&orig_SpriteDrawRect);
        api->RegisterCustomHook(0x45D660, (void*)hook_SpriteRenderQuad, (void**)&orig_SpriteRenderQuad);
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