#include "Hud.h"

#include <string>
#include <sstream>
#include <vector>
#include <math.h>
using namespace std;

#include "Settings.h"

HUD::HUD(IDirect3DDevice9 *device, int width, int height) 
	: Effect(device), width(width), height(height) {

	DWORD flags = D3DXFX_NOT_CLONEABLE;

	// Load effect from file
	SDLOG(0, "Hud Effect load\n");
	ID3DXBuffer* errors;
	HRESULT hr = D3DXCreateEffectFromFile(device, GetDirectoryFile("dsfix\\HUD.fx"), NULL, NULL, flags, NULL, &effect, &errors);
	if(hr != D3D_OK) SDLOG(0, "ERRORS:\n %s\n", errors->GetBufferPointer());
		
	// get handles
	frameTexHandle = effect->GetParameterByName(NULL, "frameTex2D");
	opacityHandle = effect->GetParameterByName(NULL, "opacity");
}

HUD::~HUD() {
	SAFERELEASE(effect);
}

HudLayout getHudLayout() {
	HudLayout l;
	l.tlH = 0.21f;
	l.blTop = 0.5f;
	l.brTop = Settings::get().getHudBottomRightTop();
	l.brLeft = Settings::get().getHudBottomRightLeft();
	l.blW = Settings::get().getHudBottomLeftWidth();
	if (l.blW < 0.05f) l.blW = 0.05f;
	if (l.brTop < 0.6f) l.brTop = 0.6f;
	if (l.brTop > 0.99f) l.brTop = 0.99f;
	if (l.brLeft < 0.45f) l.brLeft = 0.45f;
	if (l.brLeft > 0.98f) l.brLeft = 0.98f;
	if (l.blW > l.brLeft - 0.01f) l.blW = l.brLeft - 0.01f;
	return l;
}

void HUD::go(IDirect3DTexture9* input, IDirect3DSurface9* dst) {
	device->SetVertexDeclaration(vertexDeclaration);
	device->SetRenderTarget(0, dst);
	effect->SetTexture(frameTexHandle, input);

	// Fetch individual scale factors.
	// Scales below 1.0 are applied per draw call (see RSManager::redirectDraw*PrimitiveUP), so they must not be applied a
	// second time here. Only scales above 1.0 are still done by this compositor.
	float scaleTL = Settings::get().getHudScaleTopLeft();
	float scaleBL = Settings::get().getHudScaleBottomLeft();
	float scaleBR = Settings::get().getHudScaleBottomRight();
	if (scaleTL < 1.0f) scaleTL = 1.0f;
	if (scaleBL < 1.0f) scaleBL = 1.0f;
	if (scaleBR < 1.0f) scaleBR = 1.0f;
	float iscaleTL = 1.0f - scaleTL;
	float iscaleBL = 1.0f - scaleBL;
	float iscaleBR = 1.0f - scaleBR;

	// ---- Region layout, in normalized HUD-buffer coordinates ----
	// TL = full-width strip at the top, BL = left block at the bottom, BR = lower right corner.
	// Everything NOT inside one of these three regions is copied 1:1 below, so nothing that the game
	// drew into the HUD buffer (boss bar ruler, prompts, curse icon, ...) is silently thrown away.
	const HudLayout layout = getHudLayout();
	const float tlH = layout.tlH, blTop = layout.blTop, blW = layout.blW, brLeft = layout.brLeft, brTop = layout.brTop;

	UINT passes;
	effect->Begin(&passes, 0);

	// 0) everything outside the three scaled regions, untouched (1:1, point filtered, full opacity)
	effect->SetFloat(opacityHandle, 1.0f);
	effect->BeginPass(0);
	device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
	device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
	// band between the top strip and the bottom blocks (full width)
	rect(0.0f, tlH, 1.0f, blTop - tlH,
		0.0f, tlH, 1.0f, blTop - tlH);
	// right of BL, above BR
	rect(blW, blTop, 1.0f - blW, brTop - blTop,
		blW, blTop, 1.0f - blW, brTop - blTop);
	// bottom, between BL and BR (boss bar lives around here)
	rect(blW, brTop, brLeft - blW, 1.0f - brTop,
		blW, brTop, brLeft - blW, 1.0f - brTop);
	effect->EndPass();

	// upper left (Health/Stamina)
	effect->SetFloat(opacityHandle, Settings::get().getHudTopLeftOpacity());
	effect->BeginPass(0);
	device->SetSamplerState(0, D3DSAMP_MINFILTER, scaleTL == 1.0f ? D3DTEXF_POINT : D3DTEXF_LINEAR);
	device->SetSamplerState(0, D3DSAMP_MAGFILTER, scaleTL == 1.0f ? D3DTEXF_POINT : D3DTEXF_LINEAR);
	rect(0.0f, 0.0f, 1.0f, tlH,
		0.0f, 0.0f, 1.0f * scaleTL, tlH * scaleTL);
	effect->EndPass();

	// lower left (Items/Weapons)
	effect->SetFloat(opacityHandle, Settings::get().getHudBottomLeftOpacity());
	effect->BeginPass(0);
	device->SetSamplerState(0, D3DSAMP_MINFILTER, scaleBL == 1.0f ? D3DTEXF_POINT : D3DTEXF_LINEAR);
	device->SetSamplerState(0, D3DSAMP_MAGFILTER, scaleBL == 1.0f ? D3DTEXF_POINT : D3DTEXF_LINEAR);
	rect(0.0f, blTop, blW, 1.0f - blTop,
		0.0f, blTop + (1.0f - blTop) * iscaleBL, blW * scaleBL, (1.0f - blTop) * scaleBL);
	effect->EndPass();

	// lower right (Souls count)
	effect->SetFloat(opacityHandle, Settings::get().getHudBottomRightOpacity());
	effect->BeginPass(0);
	device->SetSamplerState(0, D3DSAMP_MINFILTER, scaleBR == 1.0f ? D3DTEXF_POINT : D3DTEXF_LINEAR);
	device->SetSamplerState(0, D3DSAMP_MAGFILTER, scaleBR == 1.0f ? D3DTEXF_POINT : D3DTEXF_LINEAR);
	rect(brLeft, brTop, 1.0f - brLeft, 1.0f - brTop,
		brLeft + (1.0f - brLeft) * iscaleBR, brTop + (1.0f - brTop) * iscaleBR, (1.0f - brLeft) * scaleBR, (1.0f - brTop) * scaleBR);
	effect->EndPass();

	effect->End();
}

void HUD::rect(float srcLeft, float srcTop, float srcWidth, float srcHeight,
	float trgLeft, float trgTop, float trgWidth, float trgHeight) {

	// Calculate the DX9 half-pixel offset
	float offsetX = 1.0f / float(width);
	float offsetY = 1.0f / float(height);

	// Apply the offset to the target coordinates
	trgTop = -(trgTop * 2.0f - 1.0f) + offsetY;
	trgLeft = (trgLeft * 2.0f - 1.0f) - offsetX;

	float trgRight = trgLeft + trgWidth * 2.0f;
	float trgBottom = trgTop - trgHeight * 2.0f;
	float srcRight = srcLeft + srcWidth;
	float srcBottom = srcTop + srcHeight;

	float quad[4][5] = {
			{ trgLeft,  trgTop,    0.5f, srcLeft,  srcTop    },
			{ trgRight, trgTop,    0.5f, srcRight, srcTop    },
			{ trgLeft,  trgBottom, 0.5f, srcLeft,  srcBottom },
			{ trgRight, trgBottom, 0.5f, srcRight, srcBottom }
	};
	device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
}
