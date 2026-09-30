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

void HUD::go(IDirect3DTexture9* input, IDirect3DSurface9* dst) {
	device->SetVertexDeclaration(vertexDeclaration);
	device->SetRenderTarget(0, dst);
	effect->SetTexture(frameTexHandle, input);

	// Fetch individual scale factors
	float scaleTL = Settings::get().getHudScaleTopLeft();
	float iscaleTL = 1.0f - scaleTL;

	float scaleBL = Settings::get().getHudScaleBottomLeft();
	float iscaleBL = 1.0f - scaleBL;

	float scaleBR = Settings::get().getHudScaleBottomRight();
	float iscaleBR = 1.0f - scaleBR;

	UINT passes;
	effect->Begin(&passes, 0);

	// upper left (Health/Stamina)
	effect->SetFloat(opacityHandle, Settings::get().getHudTopLeftOpacity());
	effect->BeginPass(0);
	device->SetSamplerState(0, D3DSAMP_MINFILTER, scaleTL == 1.0f ? D3DTEXF_POINT : D3DTEXF_LINEAR);
	device->SetSamplerState(0, D3DSAMP_MAGFILTER, scaleTL == 1.0f ? D3DTEXF_POINT : D3DTEXF_LINEAR);
	rect(0.0f, 0.0f, 1.0f, 0.21f,
		0.0f, 0.0f, 1.0f * scaleTL, 0.21f * scaleTL);
	effect->EndPass();

	// lower left (Items/Weapons)
	effect->SetFloat(opacityHandle, Settings::get().getHudBottomLeftOpacity());
	effect->BeginPass(0);
	device->SetSamplerState(0, D3DSAMP_MINFILTER, scaleBL == 1.0f ? D3DTEXF_POINT : D3DTEXF_LINEAR);
	device->SetSamplerState(0, D3DSAMP_MAGFILTER, scaleBL == 1.0f ? D3DTEXF_POINT : D3DTEXF_LINEAR);
	if (Settings::get().getEnableMinimalHud()) {
		rect(0.145f, 0.527f, 0.074f, 0.204f,
			0.1f * scaleBL, 0.77f + 0.2f * iscaleBL, 0.074f * scaleBL, 0.204f * scaleBL);
		rect(0.145f, 0.731f, 0.074f, 0.204f,
			0.1f * scaleBL + 0.074f * scaleBL + 0.01f, 0.77f + 0.2f * iscaleBL, 0.074f * scaleBL, 0.204f * scaleBL);
	}
	else {
		rect(0.0f, 0.5f, 0.5f, 0.5f,
			0.0f, 0.5f + 0.5f * iscaleBL, 0.5f * scaleBL, 0.5f * scaleBL);
	}
	effect->EndPass();

	// lower right (Souls count)
	effect->SetFloat(opacityHandle, Settings::get().getHudBottomRightOpacity());
	effect->BeginPass(0);
	device->SetSamplerState(0, D3DSAMP_MINFILTER, scaleBR == 1.0f ? D3DTEXF_POINT : D3DTEXF_LINEAR);
	device->SetSamplerState(0, D3DSAMP_MAGFILTER, scaleBR == 1.0f ? D3DTEXF_POINT : D3DTEXF_LINEAR);
	rect(0.8f, 0.8f, 0.2f, 0.2f,
		0.8f + 0.2f * iscaleBR, 0.8f + 0.2f * iscaleBR, 0.2f * scaleBR, 0.2f * scaleBR);
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
