#include "HamsterballAPI.h"
#include <windows.h>

class KeyboardForceMod : public HamsterballAPI {
private:
    IModAPI* api = nullptr;

    static constexpr float KB_FORCE_DEFAULT = 0.12f;
    static constexpr float KB_FORCE_MIN = 0.10f;
    static constexpr float KB_FORCE_MAX = 0.26f;

public:
    const char* GetModName() override { return "Keyboard Force"; }
    const char* GetAuthorName() override { return "BookwormKevin"; }
    const char* GetContributors() override { return "Hamsterbot"; }
    int GetApiVersion() override { return HAMSTERBALL_API_VERSION; }

    void Initialize(IModAPI* modApi) override {
        api = modApi;

        CustomSlider slider("KB_FORCE", "KEYBOARD FORCE", KB_FORCE_DEFAULT);
        slider.stepSize = 0.01;
        slider.lowerBound = KB_FORCE_MIN;
        slider.upperBound = KB_FORCE_MAX;
        api->CreateSlider(slider, this);
    }

    void onGameUpdate() override {
        if (!api) return;

        float value = api->GetSliderState("KB_FORCE");
        if (value < KB_FORCE_MIN) value = KB_FORCE_MIN;
        if (value > KB_FORCE_MAX) value = KB_FORCE_MAX;

        api->PatchMemory(0x4D03B8, (const char*)&value, sizeof(float));
    }
};

extern "C" __declspec(dllexport) HamsterballAPI* CreateModInstance() {
    return new KeyboardForceMod();
}