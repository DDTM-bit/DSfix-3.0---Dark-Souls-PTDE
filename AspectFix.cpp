#include "AspectFix.h"
#include "Settings.h"
#include "main.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace {
    constexpr float kVanillaAspect = 16.0f / 9.0f;
    constexpr float kVanillaFovY = 0.75049156f;
    constexpr float kWiderThan = 1.70f;

    bool g_active = false;
    float g_aspect = kVanillaAspect;

    // Screens taller than 16:9 (16:10, 4:3), Vert+: every perspective the game draws with is
    // built by one function, 0x453720 (vertical field of view in xmm0, then aspect, near and far on the stack). The screen
    // cameras ask for 16:9; on a taller screen they get the screen's aspect and a vertical view opened so the horizontal
    // one stays the same. The world, the effects and the game's own screen positions (lock-on marker, enemy health bars)
    // then all agree. The game's 16:9 letterbox viewports, blits and Present are opened to the whole target (below), so
    // the 2D UI covers the whole frame as well.
    // (The earlier method, which wrote the camera constants and corrected 16:9 projection matrices as they were sent to
    // the GPU, zoomed the world a little and cut off its borders; it was removed.)
    constexpr DWORD kPerspective = 0x00453720;
    const BYTE kPerspectivePrologue[9] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x70 }; // push ebp; mov ebp,esp; and esp,-16; sub esp,70h
    void* g_perspOrig = nullptr;

    void __cdecl adjust_perspective(float* fov, float* aspect) {
        if (!g_active || std::abs(*aspect - kVanillaAspect) >= 0.01f) return; // shadow and light cameras use other aspects
        const float f = *fov;
        if (!(f > 0.05f && f < 3.0f)) return;
        *fov = 2.0f * std::atan(std::tan(f * 0.5f) * kVanillaAspect / g_aspect);
        *aspect = g_aspect;
    }

    // All general registers, the flags and xmm0-xmm7 are kept; only the field of view (xmm0) and the aspect on the
    // stack change.
    __declspec(naked) void hk_perspective() {
        __asm {
            pushfd
            pushad
            sub esp, 0x80
            movdqu [esp], xmm0
            movdqu [esp + 0x10], xmm1
            movdqu [esp + 0x20], xmm2
            movdqu [esp + 0x30], xmm3
            movdqu [esp + 0x40], xmm4
            movdqu [esp + 0x50], xmm5
            movdqu [esp + 0x60], xmm6
            movdqu [esp + 0x70], xmm7
            lea eax, [esp + 0xA8]   // the aspect: past the saved xmm (0x80), pushad (0x20), pushfd and the return address
            push eax
            lea eax, [esp + 4]      // the saved xmm0: the vertical field of view
            push eax
            call adjust_perspective
            add esp, 8
            movdqu xmm0, [esp]
            movdqu xmm1, [esp + 0x10]
            movdqu xmm2, [esp + 0x20]
            movdqu xmm3, [esp + 0x30]
            movdqu xmm4, [esp + 0x40]
            movdqu xmm5, [esp + 0x50]
            movdqu xmm6, [esp + 0x60]
            movdqu xmm7, [esp + 0x70]
            add esp, 0x80
            popad
            popfd
            jmp dword ptr[g_perspOrig]
        }
    }

    bool install_projection_hook() {
        BYTE* site = (BYTE*)kPerspective;
        if (memcmp(site, kPerspectivePrologue, sizeof(kPerspectivePrologue)) != 0) {
            SDLOG(0, "AspectFix: projection function 0x%08X does not match this build\n", kPerspective);
            return false;
        }
        const size_t kStolen = sizeof(kPerspectivePrologue); // no relative operands: can be moved as they are
        BYTE* tramp = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!tramp) return false;
        memcpy(tramp, site, kStolen);
        tramp[kStolen] = 0xE9;
        const int32_t back = (int32_t)((kPerspective + kStolen) - ((DWORD)tramp + kStolen + 5));
        memcpy(tramp + kStolen + 1, &back, 4);
        FlushInstructionCache(GetCurrentProcess(), tramp, 32);
        g_perspOrig = tramp;

        DWORD oldProtect;
        if (!VirtualProtect(site, kStolen, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
        site[0] = 0xE9;
        const int32_t rel = (int32_t)((DWORD)&hk_perspective - (kPerspective + 5));
        memcpy(site + 1, &rel, 4);
        memset(site + 5, 0x90, kStolen - 5);
        FlushInstructionCache(GetCurrentProcess(), site, kStolen);
        VirtualProtect(site, kStolen, oldProtect, &oldProtect);
        return true;
    }

    // last viewport the game requested and the one actually set after correction
    bool g_vpCorrected = false;
    bool g_vpLogged = false;
    D3DVIEWPORT9 g_vpGame = {};
    D3DVIEWPORT9 g_vpUsed = {};

    bool same_vp(const D3DVIEWPORT9& a, const D3DVIEWPORT9& b) {
        return a.X == b.X && a.Y == b.Y && a.Width == b.Width && a.Height == b.Height;
    }

    bool near_f(float a, float b, float tol = 0.04f) {
        return std::abs(a - b) < tol;
    }

    float vert_plus_fovy(float aspect) {
        // tan(newFov/2) = tan(oldFov/2) * (16/9) / aspect
        const float t = std::tan(kVanillaFovY * 0.5f) * kVanillaAspect / aspect;
        return 2.0f * std::atan(t);
    }

    bool is_letterbox(LONG x, LONG y, LONG w, LONG h, LONG bw, LONG bh) {
        if (bw < 16 || bh < 16 || w < 16 || h < 16) return false;
        if (w * 10 < bw * 9 || x > bw / 25) return false;
        if (h + 8 >= bh) return false;

        const float vp = static_cast<float>(w) / static_cast<float>(h);
        if (!near_f(vp, kVanillaAspect)) return false;

        const float bb = static_cast<float>(bw) / static_cast<float>(bh);
        if (!(bb < kWiderThan)) return false;

        const LONG bars = bh - h;
        const LONG centered = bars / 2;
        const LONG dy = y - centered;
        return dy < 8 && dy > -8;
    }

    bool target_size(IDirect3DSurface9* surface, UINT* w, UINT* h) {
        if (!surface) return false;
        D3DSURFACE_DESC desc{};
        if (FAILED(surface->GetDesc(&desc))) return false;
        *w = desc.Width;
        *h = desc.Height;
        return true;
    }
}

bool installAspectFix() {
    float width = static_cast<float>(Settings::get().getRenderWidth());
    float height = static_cast<float>(Settings::get().getRenderHeight());

    if (Settings::get().getPresentWidth() > 0 && Settings::get().getPresentHeight() > 0) {
        width = static_cast<float>(Settings::get().getPresentWidth());
        height = static_cast<float>(Settings::get().getPresentHeight());
    }

    if (height <= 0.0f) return false;

    float aspect = width / height;

    // Check if the target is taller than 16:9 (e.g., 16:10 is 1.6, 4:3 is 1.33)
    if (aspect < kWiderThan && aspect > 1.20f) {
        g_aspect = aspect;
        // g_active must be set before the hook can run; if the hook cannot be installed, nothing is changed at all and
        // the game shows its own 16:9 picture with bars, as without DSfix
        g_active = true;
        if (!install_projection_hook()) {
            g_active = false;
            SDLOG(0, "AspectFix: not possible on this build, the game keeps its 16:9 picture\n");
            return false;
        }
        SDLOG(0, "AspectFix: 16:10 / Vert+ through the game's projection (Aspect: %.3f, vertical view %.1f -> %.1f deg)\n",
            aspect, kVanillaFovY * 57.29578f, vert_plus_fovy(aspect) * 57.29578f);
        return true;
    }

    g_active = false;
    return true;
}

bool isAspectActive() {
    return g_active;
}

float getAspectRatio() {
    return g_aspect;
}

void correctViewport(D3DVIEWPORT9* vp, IDirect3DDevice9* device) {
    if (!g_active || !vp || !device) return;

    IDirect3DSurface9* target = nullptr;
    UINT bw = 0, bh = 0;
    if (SUCCEEDED(device->GetRenderTarget(0, &target)) && target_size(target, &bw, &bh)) {
        if (is_letterbox(static_cast<LONG>(vp->X), static_cast<LONG>(vp->Y),
            static_cast<LONG>(vp->Width), static_cast<LONG>(vp->Height),
            static_cast<LONG>(bw), static_cast<LONG>(bh))) {
            vp->X = 0;
            vp->Y = 0;
            vp->Width = bw;
            vp->Height = bh;
        }
    }
    if (target) target->Release();
}

const RECT* correctStretchRect(IDirect3DSurface9* dst, const RECT* dstRect, RECT* outRect) {
    if (!g_active || !dst || !dstRect || !outRect) return dstRect;

    UINT bw = 0, bh = 0;
    const LONG w = dstRect->right - dstRect->left;
    const LONG h = dstRect->bottom - dstRect->top;

    if (target_size(dst, &bw, &bh) &&
        is_letterbox(dstRect->left, dstRect->top, w, h, static_cast<LONG>(bw), static_cast<LONG>(bh))) {
        outRect->left = 0;
        outRect->top = 0;
        outRect->right = static_cast<LONG>(bw);
        outRect->bottom = static_cast<LONG>(bh);
        return outRect;
    }
    return dstRect;
}

void noteViewport(const D3DVIEWPORT9& game, const D3DVIEWPORT9& used) {
    g_vpGame = game;
    g_vpUsed = used;
    g_vpCorrected = g_active && !same_vp(game, used);
    if (g_vpCorrected && !g_vpLogged) {
        g_vpLogged = true;
        SDLOG(0, "AspectFix: letterbox viewport %lu/%lu %lux%lu expanded to %lu/%lu %lux%lu\n",
            game.X, game.Y, game.Width, game.Height, used.X, used.Y, used.Width, used.Height);
    }
}

bool getGameViewport(const D3DVIEWPORT9& current, D3DVIEWPORT9* out) {
    if (!g_active || !g_vpCorrected || !out) return false;
    // D3D resets the viewport on every SetRenderTarget, so only trust the record while it still matches the device
    if (!same_vp(current, g_vpUsed)) return false;
    *out = g_vpGame;
    return true;
}

const RECT* correctPresentRect(IDirect3DDevice9* device, HWND overrideWindow, const RECT* dst) {
    if (!g_active || !dst || !device) return dst;
    HWND target = overrideWindow;
    if (!target) {
        D3DDEVICE_CREATION_PARAMETERS created = {};
        if (SUCCEEDED(device->GetCreationParameters(&created))) target = created.hFocusWindow;
    }
    if (!target) return dst;
    RECT client = {};
    if (!GetClientRect(target, &client)) return dst;
    const LONG bw = client.right - client.left, bh = client.bottom - client.top;
    if (is_letterbox(dst->left, dst->top, dst->right - dst->left, dst->bottom - dst->top, bw, bh)) {
        static bool logged = false;
        if (!logged) { logged = true; SDLOG(0, "AspectFix: Present was letterboxed; using the full window\n"); }
        return NULL;
    }
    return dst;
}
