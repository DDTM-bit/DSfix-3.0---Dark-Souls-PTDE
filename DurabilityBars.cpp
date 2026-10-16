#include "DurabilityBars.h"
#include "GameParams.h"
#include "Settings.h" // SDLOG reads the log level when RELEASE_VER is not defined
#include "main.h"
#include <cstdint>
#include <cstring>
#include <cmath>

// =====================================================================================================================
// LOOK OF THE DURABILITY BARS - everything that decides how the bars look is in this block. Changing values here does
// not affect anything else. Positions and sizes are fractions of the HUD (the game's 1280x720 UI space: x 0..1 is the
// width, y 0..1 the height), measured from the weapon slot's frame, so the bars follow the slot wherever it is drawn.
// Colours are D3DCOLOR_ARGB(alpha, red, green, blue), 0..255.
// =====================================================================================================================
namespace DurabilityBarLook {
    const float kGapBelowFrame = 0.0170f; // from the bottom of the slot's frame to the top of the bar (just under the slot box)
    const float kHeight        = 0.0050f; // thickness of the bar (about 5 pixels at 1080p)
    const float kInsetX        = 0.0030f; // margin on the left and right, inside the frame's width
    const float kBorder        = 0.0010f; // dark edge around the red part (as a part of the screen height; at least 1 pixel)
    const D3DCOLOR kBackground = D3DCOLOR_ARGB(235, 22, 17, 17);  // the dark bar that stays when durability drops
    const D3DCOLOR kFill       = D3DCOLOR_ARGB(255, 112, 52, 54); // the dark red part that shrinks (Dark Souls 3 style)
    // How much the bar fades while the game fades the weapon picture (the shield while two-handing: the picture is drawn at
    // about 18 % then). 1.0 = exactly as much as the picture (a dark bar at 18 % is almost invisible); smaller values fade
    // the bar less: 0.5 = about 43 % at the two-handing fade. Always fully visible when the picture is fully visible.
    const float kFadeCurve     = 0.95f;
    // The arrow / bolt boxes under a bow or crossbow slot are moved down so that they start this far below the bar
    // (their count number moves with them).
    const float kAmmoGapBelowBar = 0.0010f;
}
// =====================================================================================================================

// Where the game keeps the durability (research: the Dark Souls Overhaul team, metal-crow & Sean Pesce):
//  * The equipped weapons: [[[0x1378700] + 0x8] + 0x318] + 0x78 is a block with "the left hand uses its second weapon"
//    at +0x04, the same for the right hand at +0x08, and the weapon ids of the four slots at +0x1C (left 1), +0x20
//    (right 1), +0x24 (left 2) and +0x28 (right 2).
//  * The current durability: the game reads it for an equipped weapon at 0xFAB8D8 (lea ebp, [edi*8+0]) with esi
//    pointing at the slot's weapon id and eax holding the durability. A hook there notes the value and the weapon id.
//  * The maximum durability: the weapon's row in the weapon parameter table (GameParams.cpp).
//
// Drawing: the HUD draws each weapon slot as its background, the pedestal, the weapon picture and the frame on top
// (seen in F9 dumps). Right after the frame, the bar is drawn into the same target with the same viewport, so DSfix's
// bottom-left scaling and the HUD buffer's opacity apply to it exactly as to the slot. An empty hand draws no weapon
// picture, so it gets no bar. While two-handing, the game draws the other hand's picture faded (vertex alpha); the bar
// takes the same alpha.

namespace {
    // The two weapon frames in HUD fractions (left hand, right hand), from F9 dumps: centre and size.
    struct SlotRef { float cx, cy, w, h; };
    const SlotRef kSlots[2] = {
        { 0.1079f, 0.7194f, 0.0703f, 0.1389f }, // left:  [0.073 0.650 0.143 0.789]
        { 0.2559f, 0.7194f, 0.0711f, 0.1389f }, // right: [0.220 0.650 0.291 0.789]
    };
    const float kSlotTolerance = 0.008f;

    // Hook data, written by the game's thread in the hook, read when drawing.
    const DWORD kHookSite = 0x00FAB8D8;
    const BYTE kHookExpect[7] = { 0x8D, 0x2C, 0xFD, 0x00, 0x00, 0x00, 0x00 }; // lea ebp, [edi*8+0]
    DWORD g_hookBack = 0x00FAB8DF;
    volatile DWORD g_slotAddr[4] = { 0, 0, 0, 0 }; // addresses of the four slot ids (0 = not known yet)
    volatile DWORD g_slotDur[4] = { 0, 0, 0, 0 };  // last durability the game read for that slot
    volatile DWORD g_slotId[4] = { 0, 0, 0, 0 };   // the weapon id it was read for
    bool g_installed = false;

    // Alpha of the weapon picture seen in each slot this frame (-1 = none: empty hand, or not drawn yet).
    int g_pictureAlpha[2] = { -1, -1 };

    // The arrow / bolt box of a hand (bow or crossbow equipped), from F9 dumps. It starts with a backdrop quad (health-bar
    // texture) right under the weapon slot, then its pieces and the count number follow, all inside the region.
    const float kAmmoBackdrop[2][4] = {
        { 0.071f, 0.803f, 0.115f, 0.889f }, // left hand
        { 0.218f, 0.803f, 0.262f, 0.889f }, // right hand
    };
    const float kAmmoRegion[2][4] = {
        { 0.066f, 0.798f, 0.150f, 0.894f },
        { 0.212f, 0.798f, 0.296f, 0.894f },
    };
    const float kAmmoBackdropTolerance = 0.006f;
    const float kFrameBottom = 0.7889f; // bottom of the weapon slot frames
    int g_ammoSequence = -1; // hand whose arrow / bolt box is being drawn right now, -1 = none

    // How far the arrow / bolt boxes move down, in the game's UI units (1280x720 x 8).
    int ammoShiftUnits() {
        using namespace DurabilityBarLook;
        const float shift = kFrameBottom + kGapBelowFrame + kHeight + kAmmoGapBelowBar - kAmmoBackdrop[0][1];
        return shift > 0.0f ? (int)(shift * 5760.0f + 0.5f) : 0;
    }

    __declspec(naked) void durability_hook() {
        __asm {
            pushfd
            test esi, esi
            jz done
            cmp esi, dword ptr[g_slotAddr]
            je s0
            cmp esi, dword ptr[g_slotAddr + 4]
            je s1
            cmp esi, dword ptr[g_slotAddr + 8]
            je s2
            cmp esi, dword ptr[g_slotAddr + 12]
            je s3
            jmp done
        s0:
            push ecx
            mov ecx, dword ptr[esi]
            mov dword ptr[g_slotId], ecx
            mov dword ptr[g_slotDur], eax
            pop ecx
            jmp done
        s1:
            push ecx
            mov ecx, dword ptr[esi]
            mov dword ptr[g_slotId + 4], ecx
            mov dword ptr[g_slotDur + 4], eax
            pop ecx
            jmp done
        s2:
            push ecx
            mov ecx, dword ptr[esi]
            mov dword ptr[g_slotId + 8], ecx
            mov dword ptr[g_slotDur + 8], eax
            pop ecx
            jmp done
        s3:
            push ecx
            mov ecx, dword ptr[esi]
            mov dword ptr[g_slotId + 12], ecx
            mov dword ptr[g_slotDur + 12], eax
            pop ecx
        done:
            popfd
            lea ebp, [edi * 8]                // the instruction the jump replaced
            jmp dword ptr[g_hookBack]
        }
    }

    struct Equip {
        DWORD addr[4];  // addresses of the slot ids: left 1, right 1, left 2, right 2
        DWORD id[4];
        bool secondL, secondR;
    };

    // Reads the equipped weapons. No C++ objects in here (__try).
    bool readEquip(Equip* e) {
        __try {
            const DWORD a = *(const DWORD*)0x01378700;
            if (a < 0x10000) return false;
            const DWORD b = *(const DWORD*)(a + 0x8);
            if (b < 0x10000) return false;
            const DWORD c = *(const DWORD*)(b + 0x318);
            if (c < 0x10000) return false;
            const DWORD blk = c + 0x78;
            e->secondL = *(const BYTE*)(blk + 0x04) != 0;
            e->secondR = *(const BYTE*)(blk + 0x08) != 0;
            for (int i = 0; i < 4; ++i) {
                e->addr[i] = blk + 0x1C + 4 * i;
                e->id[i] = *(const DWORD*)e->addr[i];
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool matchSlot(const float box[4], int slot) {
        const SlotRef& r = kSlots[slot];
        const float cx = (box[0] + box[2]) * 0.5f, cy = (box[1] + box[3]) * 0.5f;
        const float w = box[2] - box[0], h = box[3] - box[1];
        return fabsf(cx - r.cx) < kSlotTolerance && fabsf(cy - r.cy) < kSlotTolerance &&
            fabsf(w - r.w) < kSlotTolerance && fabsf(h - r.h) < kSlotTolerance;
    }

    // The weapon picture: inside the slot's frame and at least half its height (the pedestal under it is flat, the slot
    // background is larger than the frame).
    bool isSlotPicture(const float box[4], int slot) {
        const SlotRef& r = kSlots[slot];
        const float x1 = r.cx - r.w * 0.5f, x2 = r.cx + r.w * 0.5f, y1 = r.cy - r.h * 0.5f, y2 = r.cy + r.h * 0.5f;
        const float eps = 0.0015f;
        return box[0] >= x1 - eps && box[2] <= x2 + eps && box[1] >= y1 - eps && box[3] <= y2 + eps &&
            (box[3] - box[1]) >= r.h * 0.5f;
    }

    D3DCOLOR scaleAlpha(D3DCOLOR c, float f) {
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        const DWORD a = (DWORD)((float)(c >> 24) * f + 0.5f);
        return (c & 0x00FFFFFF) | (a << 24);
    }

    struct BarVertex { float x, y, z, rhw; D3DCOLOR color; };
    const DWORD kBarFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;

    void addRect(BarVertex* v, int& n, float x1, float y1, float x2, float y2, D3DCOLOR c) {
        // pixel centres: D3D9 pre-transformed vertices need the half-pixel offset
        x1 -= 0.5f; y1 -= 0.5f; x2 -= 0.5f; y2 -= 0.5f;
        const BarVertex q[6] = {
            { x1, y1, 0.0f, 1.0f, c }, { x2, y1, 0.0f, 1.0f, c }, { x1, y2, 0.0f, 1.0f, c },
            { x2, y1, 0.0f, 1.0f, c }, { x2, y2, 0.0f, 1.0f, c }, { x1, y2, 0.0f, 1.0f, c },
        };
        memcpy(v + n, q, sizeof(q));
        n += 6;
    }

    // Saves the device state the bar changes, and puts it back afterwards.
    struct StateGuard {
        IDirect3DDevice9* d;
        IDirect3DVertexShader9* vs;
        IDirect3DPixelShader9* ps;
        IDirect3DVertexDeclaration9* decl;
        DWORD fvf;
        IDirect3DBaseTexture9* tex;
        DWORD c0op, c0a1, a0op, a0a1, c1op;
        DWORD blend, src, dst, bop, sepAlpha, srcA, dstA, bopA, atest, zen, cull, fog, stencil;
        StateGuard(IDirect3DDevice9* dev) : d(dev), vs(NULL), ps(NULL), decl(NULL), tex(NULL) {
            d->GetVertexShader(&vs);
            d->GetPixelShader(&ps);
            d->GetVertexDeclaration(&decl);
            d->GetFVF(&fvf);
            d->GetTexture(0, &tex);
            d->GetTextureStageState(0, D3DTSS_COLOROP, &c0op);
            d->GetTextureStageState(0, D3DTSS_COLORARG1, &c0a1);
            d->GetTextureStageState(0, D3DTSS_ALPHAOP, &a0op);
            d->GetTextureStageState(0, D3DTSS_ALPHAARG1, &a0a1);
            d->GetTextureStageState(1, D3DTSS_COLOROP, &c1op);
            d->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
            d->GetRenderState(D3DRS_SRCBLEND, &src);
            d->GetRenderState(D3DRS_DESTBLEND, &dst);
            d->GetRenderState(D3DRS_BLENDOP, &bop);
            d->GetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, &sepAlpha);
            d->GetRenderState(D3DRS_SRCBLENDALPHA, &srcA);
            d->GetRenderState(D3DRS_DESTBLENDALPHA, &dstA);
            d->GetRenderState(D3DRS_BLENDOPALPHA, &bopA);
            d->GetRenderState(D3DRS_ALPHATESTENABLE, &atest);
            d->GetRenderState(D3DRS_ZENABLE, &zen);
            d->GetRenderState(D3DRS_CULLMODE, &cull);
            d->GetRenderState(D3DRS_FOGENABLE, &fog);
            d->GetRenderState(D3DRS_STENCILENABLE, &stencil);
        }
        ~StateGuard() {
            d->SetVertexShader(vs);
            d->SetPixelShader(ps);
            if (decl) d->SetVertexDeclaration(decl);
            else d->SetFVF(fvf);
            d->SetTexture(0, tex);
            d->SetTextureStageState(0, D3DTSS_COLOROP, c0op);
            d->SetTextureStageState(0, D3DTSS_COLORARG1, c0a1);
            d->SetTextureStageState(0, D3DTSS_ALPHAOP, a0op);
            d->SetTextureStageState(0, D3DTSS_ALPHAARG1, a0a1);
            d->SetTextureStageState(1, D3DTSS_COLOROP, c1op);
            d->SetRenderState(D3DRS_ALPHABLENDENABLE, blend);
            d->SetRenderState(D3DRS_SRCBLEND, src);
            d->SetRenderState(D3DRS_DESTBLEND, dst);
            d->SetRenderState(D3DRS_BLENDOP, bop);
            d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, sepAlpha);
            d->SetRenderState(D3DRS_SRCBLENDALPHA, srcA);
            d->SetRenderState(D3DRS_DESTBLENDALPHA, dstA);
            d->SetRenderState(D3DRS_BLENDOPALPHA, bopA);
            d->SetRenderState(D3DRS_ALPHATESTENABLE, atest);
            d->SetRenderState(D3DRS_ZENABLE, zen);
            d->SetRenderState(D3DRS_CULLMODE, cull);
            d->SetRenderState(D3DRS_FOGENABLE, fog);
            d->SetRenderState(D3DRS_STENCILENABLE, stencil);
            if (vs) vs->Release();
            if (ps) ps->Release();
            if (decl) decl->Release();
            if (tex) tex->Release();
        }
    };

    // Draws one bar under the frame 'box' (HUD fractions). ratio: 0..1 of durability left; alpha: 0..1 multiplier.
    // hudBuffer: drawn into DSfix's HUD buffer, which is later put on screen with its alpha as the opacity.
    void drawBar(IDirect3DDevice9* dev, const float box[4], float ratio, float alpha, bool hudBuffer) {
        using namespace DurabilityBarLook;
        D3DVIEWPORT9 vp;
        if (FAILED(dev->GetViewport(&vp)) || vp.Width == 0 || vp.Height == 0) return;
        const float W = (float)vp.Width, H = (float)vp.Height;
        // outer rectangle in pixels of the current target (the viewport carries DSfix's bottom-left scaling)
        float x1 = floorf(vp.X + (box[0] + kInsetX) * W + 0.5f);
        float x2 = floorf(vp.X + (box[2] - kInsetX) * W + 0.5f);
        float y1 = floorf(vp.Y + (box[3] + kGapBelowFrame) * H + 0.5f);
        float y2 = y1 + floorf(kHeight * H + 0.5f);
        if (y2 < y1 + 3.0f) y2 = y1 + 3.0f;
        if (x2 <= x1 + 4.0f) return;
        float border = floorf(kBorder * H + 0.5f);
        if (border < 1.0f) border = 1.0f;

        BarVertex v[12];
        int n = 0;
        addRect(v, n, x1, y1, x2, y2, scaleAlpha(kBackground, alpha));
        if (ratio > 0.0f) {
            const float ix1 = x1 + border, ix2 = x2 - border, iy1 = y1 + border, iy2 = y2 - border;
            float fw = (ix2 - ix1) * (ratio > 1.0f ? 1.0f : ratio);
            if (fw < 1.0f) fw = 1.0f;
            if (iy2 > iy1) addRect(v, n, ix1, iy1, floorf(ix1 + fw + 0.5f), iy2, scaleAlpha(kFill, alpha));
        }

        StateGuard guard(dev);
        dev->SetVertexShader(NULL);
        dev->SetPixelShader(NULL);
        dev->SetFVF(kBarFVF);
        dev->SetTexture(0, NULL);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        if (hudBuffer) {
            // The buffer is put on screen as "colour x alpha + screen x (1 - alpha)". Normal blending would store the colour
            // already multiplied by alpha and the alpha multiplied by itself, so a half-transparent bar would end up
            // nearly invisible. Store the bar's own colour and alpha instead (over what is already in the buffer).
            dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
            dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
            dev->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
            dev->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
            dev->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
        }
        else {
            dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        }
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, n / 3, v, sizeof(BarVertex));
    }

    // Durability left (0..1) of the weapon in a hand (0 = left, 1 = right). False: nothing to show.
    bool handRatio(int hand, float* ratio) {
        Equip e;
        if (!readEquip(&e)) {
            for (int i = 0; i < 4; ++i) g_slotAddr[i] = 0;
            return false;
        }
        for (int i = 0; i < 4; ++i) g_slotAddr[i] = e.addr[i]; // the hook watches these slots from now on
        const int slot = hand == 0 ? (e.secondL ? 2 : 0) : (e.secondR ? 3 : 1);
        const DWORD id = e.id[slot];
        if (g_slotId[slot] != id) return false; // the game has not read this weapon's durability yet
        uint32_t maxDur = 0;
        if (!weaponMaxDurability(id, &maxDur)) return false;
        const float r = (float)g_slotDur[slot] / (float)maxDur;
        *ratio = r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r);
        return true;
    }
}

bool installDurabilityBars() {
    if (g_installed) return true;
    if (memcmp((const void*)kHookSite, kHookExpect, sizeof(kHookExpect)) != 0) {
        SDLOG(0, "durabilityBars: durability read at 0x%08X does not match this build, no durability bars\n", kHookSite);
        return false;
    }
    DWORD oldProtect;
    if (!VirtualProtect((LPVOID)kHookSite, sizeof(kHookExpect), PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    BYTE* p = (BYTE*)kHookSite;
    p[0] = 0xE9; // jmp durability_hook
    *(DWORD*)(p + 1) = (DWORD)&durability_hook - (kHookSite + 5);
    p[5] = 0x90;
    p[6] = 0x90;
    VirtualProtect((LPVOID)kHookSite, sizeof(kHookExpect), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)kHookSite, sizeof(kHookExpect));
    g_installed = true;
    SDLOG(0, "durabilityBars: durability hook installed\n");
    return true;
}

bool durabilityBarsActive() {
    return g_installed;
}

void durabilityBarsNewFrame() {
    g_pictureAlpha[0] = g_pictureAlpha[1] = -1;
    g_ammoSequence = -1;
}

int durabilityBarsAmmoShift(const float box[4], bool frameTexture) {
    if (!g_installed || !box) return 0;
    for (int hand = 0; hand < 2; ++hand) {
        const float* b = kAmmoBackdrop[hand];
        if (frameTexture && fabsf(box[0] - b[0]) < kAmmoBackdropTolerance && fabsf(box[1] - b[1]) < kAmmoBackdropTolerance &&
            fabsf(box[2] - b[2]) < kAmmoBackdropTolerance && fabsf(box[3] - b[3]) < kAmmoBackdropTolerance) {
            g_ammoSequence = hand; // an arrow / bolt box starts
            return ammoShiftUnits();
        }
    }
    if (g_ammoSequence >= 0) {
        const float* r = kAmmoRegion[g_ammoSequence];
        if (box[0] >= r[0] && box[1] >= r[1] && box[2] <= r[2] && box[3] <= r[3]) return ammoShiftUnits();
        g_ammoSequence = -1; // something else is drawn: the box is complete
    }
    return 0;
}

void durabilityBarsOnHudQuad(IDirect3DDevice9* dev, const float box[4], bool frameTexture, BYTE vertexAlpha, float opacity,
    bool hudBuffer) {
    if (!g_installed || !dev || !box) return;
    for (int hand = 0; hand < 2; ++hand) {
        if (frameTexture && matchSlot(box, hand)) {
            // the slot's frame: the last piece of the slot. Draw the bar if a weapon picture was drawn in it.
            const int pictureAlpha = g_pictureAlpha[hand];
            g_pictureAlpha[hand] = -1;
            float ratio = 0.0f;
            if (pictureAlpha > 0 && handRatio(hand, &ratio)) {
                float fade = (float)pictureAlpha / 255.0f;
                if (fade < 1.0f) fade = powf(fade, DurabilityBarLook::kFadeCurve);
                drawBar(dev, box, ratio, fade * opacity, hudBuffer);
            }
            return;
        }
        if (!frameTexture && isSlotPicture(box, hand)) {
            g_pictureAlpha[hand] = vertexAlpha; // the latest picture drawn in the slot (the weapon, after the pedestal)
            return;
        }
    }
}
