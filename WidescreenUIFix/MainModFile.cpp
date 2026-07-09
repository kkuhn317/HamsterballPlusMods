#define _CRT_SECURE_NO_WARNINGS
#include "HamsterballAPI.h"
#include <math.h>
#include <string.h>

class WidescreenUIFix : public HamsterballAPI {
private:
	IModAPI* api = nullptr;
	static inline bool g_enabled = true;

	static inline bool g_inSceneRender = false;
	static inline int g_viewportCallCount = 0;

	// Scale factor (presentParams+0x1f8) and X offset (gfx+0x798)
	static inline bool g_uiModified = false;
	static inline float g_origScaleX = 0.0f;
	static inline int g_origOffsetX = 0;
	static inline DWORD g_scaleXAddr = 0;
	static inline DWORD g_offsetXAddr = 0;

	typedef void(__fastcall* SetViewport_t)(void*, void*, int, int);
	static inline SetViewport_t orig_SetViewport = nullptr;

	typedef void(__fastcall* SceneRender_t)(void*, void*, void*);
	static inline SceneRender_t orig_SceneRender = nullptr;

	// UI coordinate system (from Ghidra decompilation):
	//   Gfx_TransformY(pixel_x) = pixel_x * scaleX + offsetX
	//   Gfx_TransformZ(pixel_y) = pixel_y * scaleY + offsetY
	//
	// Where:
	//   scaleX = *(float*)(config + 0x1f8)   (config = *(gfx+0x5c))
	//   scaleY = *(float*)(config + 0x1fc)
	//   offsetX = *(int*)(gfx + 0x798)
	//   offsetY = *(int*)(gfx + 0x79c)
	//   bbWidth = *(int*)(config + 0x15c)
	//   bbHeight = *(int*)(config + 0x160)
	//
	// Graphics_SetViewport sets:
	//   gfx+0x798 = 0  (offsetX, reset to 0 for full-screen)
	//   gfx+0x79c = 0  (offsetY)
	//   gfx+0x7a0 = (float)bbWidth  (render width)
	//   gfx+0x7a4 = (float)bbHeight (render height)
	//   Then builds the projection with aspect = renderW / renderH
	//
	// On 16:9, scaleX is calibrated for the full width, so UI stretches.
	// Fix: shrink scaleX proportionally and add X offset to center (pillarbox).
	static void fixUIScale(void* gfx) {
		DWORD gfxAddr = (DWORD)gfx;
		if (IsBadReadPtr(gfx, 0x800)) return;

		DWORD config = *(DWORD*)(gfxAddr + 0x5c);
		if (!config || IsBadReadPtr((void*)config, 0x200)) return;

		DWORD bbWidth = *(DWORD*)(config + 0x15c);
		DWORD bbHeight = *(DWORD*)(config + 0x160);
		if (bbWidth <= 0 || bbHeight <= 0) return;

		// Already 4:3 or taller — no fix needed
		float currentAspect = (float)bbWidth / (float)bbHeight;
		if (currentAspect <= 1.34f) return;

		float scaleX = *(float*)(config + 0x1f8);
		float scaleY = *(float*)(config + 0x1fc);

		// The correct scaleX for 4:3 UI proportions:
		//   On 4:3: scaleX/scaleY = 1.0 (already correct)
		//   On 16:9: scaleX/scaleY = 16:9 / 4:3 = 4/3, so scaleX is 4/3 too large
		//   Fix: newScaleX = scaleX * (4/3) / (screenAspect)
		//   = scaleX * (4.0 * bbHeight) / (3.0 * bbWidth)
		float aspectRatio = (float)bbWidth / (float)bbHeight;
		float ratio43 = 4.0f / 3.0f;
		float newScaleX = scaleX * ratio43 / aspectRatio;

		// Center the UI: shift X offset so the compressed UI is centered.
		// The UI's effective pixel width after compression = bbWidth * (newScaleX / scaleX)
		// = bbWidth * (ratio43 / aspectRatio) = bbWidth * (4/3) / (bbWidth/bbHeight)
		// = bbHeight * 4/3
		// Pillarbox margin = (bbWidth - bbHeight*4/3) / 2
		// Offset in NDC = margin * newScaleX
		float uiWidth = (float)bbHeight * ratio43;
		float marginPixels = ((float)bbWidth - uiWidth) / 2.0f;
		int newOffsetX = (int)(marginPixels * newScaleX);

		// Save originals
		g_origScaleX = scaleX;
		g_origOffsetX = *(int*)(gfxAddr + 0x798);
		g_scaleXAddr = config + 0x1f8;
		g_offsetXAddr = gfxAddr + 0x798;
		g_uiModified = true;

		// Apply
		*(float*)(config + 0x1f8) = newScaleX;
		*(int*)(gfxAddr + 0x798) = newOffsetX;
	}

	static void restoreUIScale() {
		if (g_uiModified) {
			if (g_scaleXAddr) *(float*)g_scaleXAddr = g_origScaleX;
			if (g_offsetXAddr) *(int*)g_offsetXAddr = g_origOffsetX;
			g_uiModified = false;
			g_scaleXAddr = 0;
			g_offsetXAddr = 0;
		}
	}

	static void __fastcall hook_SetViewport(void* gfx, void* edx, int param1, int param2) {
		orig_SetViewport(gfx, edx, param1, param2);

		if (!g_enabled || !g_inSceneRender) return;
		if (param1 != 0 || param2 != 0) return;

		g_viewportCallCount++;

		// 1st (0,0) = 3D pass → leave alone
		// 2nd (0,0) = UI pass → fix scale + center
		if (g_viewportCallCount == 2) {
			fixUIScale(gfx);
		}
	}

	static void __fastcall hook_SceneRender(void* this_ptr, void* edx, void* param1) {
		restoreUIScale();
		g_inSceneRender = true;
		g_viewportCallCount = 0;
		orig_SceneRender(this_ptr, edx, param1);
		g_inSceneRender = false;
		restoreUIScale();
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