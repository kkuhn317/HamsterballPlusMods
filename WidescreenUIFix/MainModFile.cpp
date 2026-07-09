#define _CRT_SECURE_NO_WARNINGS
#include "HamsterballAPI.h"
#include <math.h>
#include <string.h>

class WidescreenUIFix : public HamsterballAPI {
private:
	IModAPI* api = nullptr;
	static inline bool g_enabled = true;

	static inline bool g_inSceneRender = false;
	static inline bool g_inUIPass = false;

	// Precomputed per-frame values for the transform
	static inline float g_scaleFactor = 1.0f;
	static inline float g_margin = 0.0f;

	typedef void(__fastcall* SetViewport_t)(void*, void*, int, int);
	static inline SetViewport_t orig_SetViewport = nullptr;

	typedef void(__fastcall* SceneRender_t)(void*, void*, void*);
	static inline SceneRender_t orig_SceneRender = nullptr;

	typedef float(__fastcall* TransformY_t)(void*, void*, float);
	static inline TransformY_t orig_TransformY = nullptr;

	// UI coordinate system (from Ghidra decompilation):
	//   Gfx_TransformY(gfx, pixel_x) = pixel_x * scaleX + offsetX
	//   Gfx_TransformZ(gfx, pixel_y) = pixel_y * scaleY + offsetY
	//
	// Returns screen-space pixel coordinates (used with D3DFVF_XYZRHW).
	// On 16:9, scaleX stretches X coordinates → UI stretches.
	//
	// Fix: hook Gfx_TransformY and apply a linear transform to its output:
	//   corrected = result * scaleFactor + margin
	//
	// Where:
	//   scaleFactor = (4/3) / screenAspect = (4*bbHeight) / (3*bbWidth)
	//   margin = (bbWidth - bbHeight * 4/3) / 2
	//
	// This maps:
	//   result=0          → margin (left pillarbox edge)
	//   result=bbWidth/2  → bbWidth/2 (screen center — UNCHANGED!)
	//   result=bbWidth    → bbWidth - margin (right pillarbox edge)
	//
	// Screen center stays at screen center because:
	//   (bbWidth/2) * scaleFactor + margin
	//   = (bbWidth/2) * (bbH*4/3/bbW) + (bbW - bbH*4/3)/2
	//   = (bbH*4/3)/2 + (bbW - bbH*4/3)/2
	//   = (bbH*4/3 + bbW - bbH*4/3) / 2 = bbW/2  ✓
	//
	// This keeps centered elements (timer blot) centered while
	// properly pillarboxing left/right-aligned elements.

	static float __fastcall hook_TransformY(void* gfx, void* edx, float pixel_x) {
		float result = orig_TransformY(gfx, edx, pixel_x);
		if (!g_enabled || !g_inUIPass) return result;
		return result * g_scaleFactor + g_margin;
	}

	// Gfx_TransformY is only called by Sprite_DrawRect and similar
	// sprite functions — these are UI-only. The 3D pass uses the
	// projection matrix and vertex shaders, NOT Gfx_TransformY.
	// So we can safely apply the transform on ANY (0,0) viewport
	// call inside Scene_Render, not just the 2nd one.
	//
	// This fixes Party Race (split-screen, size==2) where there's
	// only ONE (0,0) call (the UI pass), not two.
	static void __fastcall hook_SetViewport(void* gfx, void* edx, int param1, int param2) {
		orig_SetViewport(gfx, edx, param1, param2);

		if (!g_enabled || !g_inSceneRender) return;
		if (param1 != 0 || param2 != 0) return;

		// Any (0,0) call inside Scene_Render = full-screen pass.
		// Gfx_TransformY is only used by UI sprite functions, so
		// enabling the transform here is safe for 3D too.
		g_inUIPass = true;

		DWORD gfxAddr = (DWORD)gfx;
		if (IsBadReadPtr(gfx, 0x800)) return;

		DWORD config = *(DWORD*)(gfxAddr + 0x5c);
		if (!config || IsBadReadPtr((void*)config, 0x200)) return;

		DWORD bbWidth = *(DWORD*)(config + 0x15c);
		DWORD bbHeight = *(DWORD*)(config + 0x160);
		if (bbWidth <= 0 || bbHeight <= 0) return;

		float aspect = (float)bbWidth / (float)bbHeight;
		if (aspect <= 1.34f) return; // already 4:3

		float ratio43 = 4.0f / 3.0f;
		g_scaleFactor = ratio43 / aspect;
		g_margin = ((float)bbWidth - (float)bbHeight * ratio43) / 2.0f;
	}

	static void __fastcall hook_SceneRender(void* this_ptr, void* edx, void* param1) {
		g_inUIPass = false;
		g_inSceneRender = true;
		orig_SceneRender(this_ptr, edx, param1);
		g_inSceneRender = false;
		g_inUIPass = false;
	}

public:
	const char* GetModName() override { return "Widescreen UI Fix"; }
	const char* GetAuthorName() override { return "BookwormKevin"; }
	int GetApiVersion() override { return HAMSTERBALL_API_VERSION; }

	void Initialize(IModAPI* modApi) override {
		api = modApi;
		CustomButton btn;
		btn.id = "ws_ui_fix";
		btn.displayText = "Widescreen UI Fix";
		btn.defaultState = true;
		api->CreateToggleButton(btn, this);
		api->RegisterCustomHook(0x453e90, (void*)hook_TransformY, (void**)&orig_TransformY);
		api->RegisterCustomHook(0x454f10, (void*)hook_SetViewport, (void**)&orig_SetViewport);
		api->RegisterCustomHook(0x41a2e0, (void*)hook_SceneRender, (void**)&orig_SceneRender);
	}

	void onButtonToggle(const char* id, bool state) override {
		if (strcmp(id, "ws_ui_fix") == 0) g_enabled = state;
	}
};

extern "C" __declspec(dllexport) HamsterballAPI* CreateModInstance() {
	return new WidescreenUIFix();
}