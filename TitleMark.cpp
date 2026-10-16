#include "TitleMark.h"
#include <windows.h>
#include <d3dx9.h>
#include <cmath>

// =====================================================================================================================
// LOOK OF THE TITLE SCREEN MARK - everything that decides how it looks is in this block. Sizes are parts of the screen
// height, so it looks the same at every resolution. Colours are D3DCOLOR_ARGB(alpha, red, green, blue), 0..255.
// =====================================================================================================================
namespace TitleMarkLook {
    const char kText[]          = "DSfix 3.0";
    const char kFontName[]      = "Segoe UI";        // any font installed with Windows
    const int kFontWeight       = FW_SEMIBOLD;       // FW_NORMAL, FW_SEMIBOLD or FW_BOLD
    const float kHeight         = 0.024f;            // text height (about 26 pixels at 1080p)
    const float kMarginRight    = 0.022f;            // distance from the right edge of the screen
    const float kMarginBottom   = 0.020f;            // distance from the bottom edge of the screen
    const D3DCOLOR kColor       = D3DCOLOR_ARGB(230, 150, 205, 255); // light blue
    const D3DCOLOR kShadow      = D3DCOLOR_ARGB(150, 0, 0, 0);       // soft dark shadow, keeps it readable on bright spots
    const float kShadowOffset   = 0.0015f;           // shadow distance (at least 1 pixel)
    const float kFadeInSeconds  = 0.8f;              // fade in when the title screen appears
    const float kFadeOutSeconds = 0.3f;              // fade out when it goes away
}
// =====================================================================================================================

namespace {
    ID3DXFont* g_font = NULL;
    int g_fontHeight = 0;
    float g_alpha = 0.0f;           // current visibility, 0..1
    ULONGLONG g_lastLogoMs = 0;     // when the title logo was last set
    ULONGLONG g_lastDrawMs = 0;

    D3DCOLOR withAlpha(D3DCOLOR c, float f) {
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        const DWORD a = (DWORD)((float)(c >> 24) * f + 0.5f);
        return (c & 0x00FFFFFF) | (a << 24);
    }
}

void titleMarkLogoSeen() {
    g_lastLogoMs = GetTickCount64();
}

void titleMarkOnQuad(const float box[4]) {
    // The copyright line at the bottom of the title screen ("DARK SOULS: PREPARE TO DIE EDITION & (c)2012 NAMCO BANDAI
    // ..."), drawn in three pieces from the first frame of the title screen, while the logo animation still plays
    // (F9 dump of the main menu).
    static const float kPieces[3][4] = {
        { 0.188f, 0.894f, 0.812f, 0.925f },
        { 0.139f, 0.894f, 0.261f, 0.925f },
        { 0.812f, 0.894f, 0.984f, 0.925f },
    };
    if (!box) return;
    for (int i = 0; i < 3; ++i) {
        const float* k = kPieces[i];
        if (fabsf(box[0] - k[0]) < 0.003f && fabsf(box[1] - k[1]) < 0.003f &&
            fabsf(box[2] - k[2]) < 0.003f && fabsf(box[3] - k[3]) < 0.003f) {
            g_lastLogoMs = GetTickCount64();
            return;
        }
    }
}

void titleMarkRelease() {
    if (g_font) {
        g_font->Release();
        g_font = NULL;
    }
    g_fontHeight = 0;
}

void titleMarkDraw(IDirect3DDevice9* dev) {
    using namespace TitleMarkLook;
    if (!dev) return;
    const ULONGLONG now = GetTickCount64();
    float dt = g_lastDrawMs ? (float)(now - g_lastDrawMs) / 1000.0f : 0.0f;
    if (dt > 0.1f) dt = 0.1f; // a long frame (loading) must not jump the fade
    g_lastDrawMs = now;
    // the logo is set every frame while the title screen shows; a short gap is allowed
    const bool onTitle = g_lastLogoMs != 0 && now - g_lastLogoMs < 200;
    if (onTitle) g_alpha += dt / kFadeInSeconds;
    else g_alpha -= dt / kFadeOutSeconds;
    if (g_alpha > 1.0f) g_alpha = 1.0f;
    if (g_alpha <= 0.0f) {
        g_alpha = 0.0f;
        return;
    }

    IDirect3DSurface9* bb = NULL;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    D3DSURFACE_DESC desc;
    bb->GetDesc(&desc);
    const float H = (float)desc.Height;
    int fontHeight = (int)(kHeight * H + 0.5f);
    if (fontHeight < 10) fontHeight = 10;
    if (!g_font || g_fontHeight != fontHeight) {
        titleMarkRelease();
        if (FAILED(D3DXCreateFontA(dev, fontHeight, 0, kFontWeight, 1, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, kFontName, &g_font))) {
            g_font = NULL;
            bb->Release();
            return;
        }
        g_fontHeight = fontHeight;
    }

    // draw onto the back buffer, then put back everything that was set before
    IDirect3DSurface9* oldRT = NULL;
    IDirect3DSurface9* oldDS = NULL;
    D3DVIEWPORT9 oldVp;
    DWORD oldScissor = FALSE;
    dev->GetRenderTarget(0, &oldRT);
    dev->GetDepthStencilSurface(&oldDS);
    dev->GetViewport(&oldVp);
    dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &oldScissor);
    dev->SetRenderTarget(0, bb);
    dev->SetDepthStencilSurface(NULL); // the game's depth buffer may be smaller than the back buffer
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    if (SUCCEEDED(dev->BeginScene())) {
        int shadow = (int)(kShadowOffset * H + 0.5f);
        if (shadow < 1) shadow = 1;
        RECT r = { 0, 0, (LONG)desc.Width - (LONG)(kMarginRight * H + 0.5f), (LONG)desc.Height - (LONG)(kMarginBottom * H + 0.5f) };
        RECT rs = { r.left + shadow, r.top + shadow, r.right + shadow, r.bottom + shadow };
        const DWORD fmt = DT_RIGHT | DT_BOTTOM | DT_SINGLELINE | DT_NOCLIP;
        g_font->DrawTextA(NULL, kText, -1, &rs, fmt, withAlpha(kShadow, g_alpha));
        g_font->DrawTextA(NULL, kText, -1, &r, fmt, withAlpha(kColor, g_alpha));
        dev->EndScene();
    }
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, oldScissor);
    if (oldRT) {
        dev->SetRenderTarget(0, oldRT);
        oldRT->Release();
    }
    dev->SetDepthStencilSurface(oldDS);
    if (oldDS) oldDS->Release();
    dev->SetViewport(&oldVp);
    bb->Release();
}
