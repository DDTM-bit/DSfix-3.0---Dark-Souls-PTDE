#pragma once
#include <windows.h>
#include <d3d9.h>

bool installAspectFix();
bool isAspectActive();
float getAspectRatio();

// DirectX 9 hooks called from d3d9dev.cpp
void correctViewport(D3DVIEWPORT9* vp, IDirect3DDevice9* device);
const RECT* correctStretchRect(IDirect3DSurface9* dst, const RECT* dstRect, RECT* outRect);
bool correctMatrixUpload(const float* data, UINT count, float* out);

// Viewport bookkeeping for the letterbox correction:
// noteViewport() records what the game asked for and what was actually set.
// getGameViewport() returns true (and the game's own 16:9 viewport) if 'current' is a corrected viewport set by us.
void noteViewport(const D3DVIEWPORT9& game, const D3DVIEWPORT9& used);
bool getGameViewport(const D3DVIEWPORT9& current, D3DVIEWPORT9* out);

// Fixed-function projection (SetTransform D3DTS_PROJECTION): Vert+ version of a 16:9 perspective matrix.
// Returns true and fills 'out' only if the matrix was corrected.
bool correctProjection(const D3DMATRIX* in, D3DMATRIX* out);

// Present with a 16:9 letterbox destination rectangle inside a taller window: returns NULL (whole client area),
// otherwise the rectangle unchanged.
const RECT* correctPresentRect(IDirect3DDevice9* device, HWND overrideWindow, const RECT* dst);
