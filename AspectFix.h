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