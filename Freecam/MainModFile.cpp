#define _CRT_SECURE_NO_WARNINGS
#include "HamsterballAPI.h"
#include <cmath>

static bool g_hideUI = false;

typedef void(__fastcall* FontDrawGlyph_t)(void*, void*, const char*, int, int,
    void*, void*, void*, void*, void*);
static FontDrawGlyph_t orig_FontDrawGlyph = nullptr;

static void __fastcall hook_FontDrawGlyph(void* thisPtr, void* edx, const char* text, int x, int y,
    void* p4, void* p5, void* p6, void* p7, void* p8) {
    if (g_hideUI) return;
    orig_FontDrawGlyph(thisPtr, edx, text, x, y, p4, p5, p6, p7, p8);
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
        if (!active || !api) return;
        App* app = api->GetApp();
        if (!app || IsBadReadPtr(app, sizeof(App))) return;

        bool wasHidden = g_hideUI;
        if (wasHidden) g_hideUI = false;

        CustomText ct;
        ct.font = app->fonts.showcardGothic14;
        ct.x = 90;
        ct.y = 10;
        ct.text_color = Color(0.0f, 1.0f, 0.0f, 1.0f);
        ct.enable_shadow = true;
        api->DrawCustomText("FREECAM", ct);

        if (wasHidden) g_hideUI = true;
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