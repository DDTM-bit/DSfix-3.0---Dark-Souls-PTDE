#include "AspectFix.h"
#include "Settings.h"
#include "main.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace {
    // Engine camera baseline constants in .rdata
    constexpr DWORD kAspectAddress = 0x011E8484; // Vanilla: 1.77777779f (16:9)
    constexpr DWORD kFovYAddress = 0x011E4134; // Vanilla: 0.75049156f (~43 degrees)
    constexpr float kVanillaAspect = 16.0f / 9.0f;
    constexpr float kVanillaFovY = 0.75049156f;
    constexpr float kWiderThan = 1.70f;

    bool g_active = false;
    float g_aspect = kVanillaAspect;

    bool near_f(float a, float b, float tol = 0.04f) {
        return std::abs(a - b) < tol;
    }

    float vert_plus_fovy(float aspect) {
        // tan(newFov/2) = tan(oldFov/2) * (16/9) / aspect
        const float t = std::tan(kVanillaFovY * 0.5f) * kVanillaAspect / aspect;
        return 2.0f * std::atan(t);
    }

    bool write_float(DWORD address, float value) {
        DWORD oldProtect;
        if (VirtualProtect((LPVOID)address, sizeof(float), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            *reinterpret_cast<float*>(address) = value;
            VirtualProtect((LPVOID)address, sizeof(float), oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), (LPCVOID)address, sizeof(float));
            return true;
        }
        return false;
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

    bool correct_matrix(float* m) {
        if (!g_active || m[0] < 0.05f || m[5] < 0.05f) return false;

        auto zero = [](float v) { return std::abs(v) < 0.02f; };
        if (!zero(m[1]) || !zero(m[2]) || !zero(m[4]) || !zero(m[6]) || !zero(m[8]) || !zero(m[9])) {
            return false;
        }

        const float ratio = m[5] / m[0];
        if (!near_f(ratio, kVanillaAspect, 0.035f)) return false;

        const bool row_major = near_f(m[11], 1.0f, 0.02f) && zero(m[15]);
        const bool column_major = near_f(m[14], 1.0f, 0.02f) && zero(m[15]);
        if (!row_major && !column_major) return false;

        const float y = m[0] * g_aspect;
        if (near_f(m[5], y, 0.0001f)) return false;

        m[5] = y;
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
        g_active = true;
        g_aspect = aspect;

        const float newFov = vert_plus_fovy(aspect);

        write_float(kAspectAddress, aspect);
        write_float(kFovYAddress, newFov);

        SDLOG(0, "AspectFix: 16:10 / Vert+ aspect support activated (Aspect: %.3f, FovY: %.4f rad)\n", aspect, newFov);
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

bool correctMatrixUpload(const float* data, UINT count, float* out) {
    if (!g_active || !data || !out || count < 4 || count > 24) return false;

    std::memcpy(out, data, count * 4 * sizeof(float));
    bool changed = false;
    const UINT matrices = count / 4;
    const UINT limit = matrices > 4 ? 4 : matrices;

    for (UINT i = 0; i < limit; ++i) {
        if (correct_matrix(out + i * 16)) {
            changed = true;
        }
    }
    return changed;
}