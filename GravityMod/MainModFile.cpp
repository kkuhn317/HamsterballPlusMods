#include "HamsterballAPI.h"
#include <windows.h>
#include <cmath>

class LowGravityMod : public HamsterballAPI {
private:
    IModAPI* api = nullptr;

    static constexpr float NORMAL_GRAVITY = 0.5f;
    static constexpr float LOW_GRAVITY = 0.125f;

public:
    const char* GetModName() override { return "Gravity Mod"; }
    const char* GetAuthorName() override { return "BookwormKevin"; }
    int GetApiVersion() override { return HAMSTERBALL_API_VERSION; }

    void Initialize(IModAPI* modApi) override {
        api = modApi;

        CustomSlider gravitySlider("CHEAT_GRAV", "GRAVITY", 5);
        gravitySlider.stepSize = 0.5;
        //gravitySlider.lowerBound = 0;
        api->CreateSlider(gravitySlider, this);
    }

    void onBallUpdate(Ball* ball) override {
        if (!ball) return;

        PhysicsObject* phys = ball->physics_object;
        if (!phys) return;

        float slider = api->GetSliderState("CHEAT_GRAV");

        // Read current gravity direction (set by game's Ball_Set*Gravity functions)
        // Game uses 3 unit vectors: (0,-1,0) normal, (-1,0,0) tilted, (0,0,1) flat
        float gx = phys->gravity_x;
        float gy = phys->gravity_y;
        float gz = phys->gravity_z;

        float absX = fabsf(gx);
        float absY = fabsf(gy);
        float absZ = fabsf(gz);

        // Clear all axes, then set only the dominant one
        phys->gravity_x = 0;
        phys->gravity_y = 0;
        phys->gravity_z = 0;

        if (absY > 0.001f && absY >= absX && absY >= absZ) {
            // Y-axis gravity (normal levels — game uses -Y for down)
            phys->gravity_y = (slider < 0) ? 1.0f : -1.0f;
        }
        else if (absX > 0.001f && absX >= absZ) {
            // X-axis gravity (Odd Race walls — game uses -X for down)
            phys->gravity_x = (slider < 0) ? 1.0f : -1.0f;
        }
        else if (absZ > 0.001f) {
            // Z-axis gravity (Odd Race flat — game uses +Z for down)
            phys->gravity_z = (slider < 0) ? -1.0f : 1.0f;
        }
        else {
            // No gravity set yet, default to Y-down
            phys->gravity_y = (slider < 0) ? 1.0f : -1.0f;
        }

        // Yeah its called spin rate in the api for some reason,
        // but it really is a gravity scale property.
        // It doesn't behave well with negative values,
        // but it works much better with large values than the physics object gravity.
        // The default of it is 5.
        ball->spin_rate = fabsf(slider);
    }
};

extern "C" __declspec(dllexport) HamsterballAPI* CreateModInstance() {
    return new LowGravityMod();
}