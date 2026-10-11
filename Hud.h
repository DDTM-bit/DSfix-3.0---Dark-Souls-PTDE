#pragma once

#include "Effect.h"
#include "main.h"

// Screen layout of the HUD zones, in normalized (0..1) coordinates.
// Shared by the compositor (Hud.cpp) and the per-draw scaling (RenderstateManager.cpp).
struct HudLayout {
	float tlH;     // top strip height
	float blTop;   // bottom-left block: top edge
	float blW;     // bottom-left block: width
	float brLeft;  // bottom-right block: left edge
	float brTop;   // bottom-right block: top edge
};
HudLayout getHudLayout();

class HUD : public Effect {
public:
    HUD(IDirect3DDevice9 *device, int width, int height);
    virtual ~HUD();

	void go(IDirect3DTexture9 *input, IDirect3DSurface9 *dst);

private:
	int width, height;

	ID3DXEffect *effect;
	
	D3DXHANDLE frameTexHandle;
	D3DXHANDLE opacityHandle;

	void rect(float srcLeft, float srcTop, float srcWidth, float srcHeight,
			  float trgLeft, float trgTop, float trgWidth, float trgHeight);
};
