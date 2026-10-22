#include "SlopeSlideFix.h"
#include "PhysicsFixes.h" // g_PhysicsDeltaTime: the step the engine got this frame (seconds)
#include "main.h"         // SDLOG
#include <cstring>
#include <cstdint>

// Steep slope slides start reliably at any frame rate.
//
// THE BUG
//
// The top of a slide slope (the Bed of Chaos slide, for example) has a special "slide" collision. Every frame the
// character's collision query (fn 0x00EC20C0) sweeps a slightly larger copy of the character from its new position
// back to its old one, and each special collision it touches goes into a list. The slide one (type 21, handled at
// 0x00E8345C) sets bit 0x80000 in the action input flags [action+0x84] (and the body flag +0x1D1).
// Once per frame, fn 0x00E29870 latches those flags for the action script: bit 0x80000 becomes bit 0 of
// [action+0x43C] (the store at 0x00E298BB), which the script reads as condition 296 ("touching the slide").
// The player's action script starts the slide animation 7060 when that condition is true, and while 7060 plays it
// checks it again EVERY frame: the first frame it is false, the script leaves the slide and plays 7062, the
// "walking in the air" stumble, before the slide starts again later.
//
// At 30 FPS the sweep covers a 30 FPS frame of movement and the contact never drops while the hero passes the lip.
// At high frame rates the sweep is several times shorter, and right at the lip, while 7060 starts slowly, the query
// sometimes misses the slide collision for a single frame. Measured at 144 FPS: 7060 started, the contact dropped
// for one frame (7 ms), the script ended the slide and played 7062. At 30 FPS in the same place the contact never
// dropped and the script went on to the real slide (7061). Because it depends on one marginal frame, it does not
// happen every time.
//
// THE FIX
//
// The script's view of the slide contact is kept for one 30 FPS frame (1/30 s) after the last frame that really had
// it: "touched the slide within the last 1/30 s", which is what one 30 FPS sweep reports. At 30 FPS and below
// nothing can change (the hold is over after one frame). Only the script's copy (bit 0 of [action+0x43C]) is held;
// the physics flags are left exactly as the game computed them. Every character with an action script gets the
// same rule.
//
// HOW IT IS PATCHED (site checked byte for byte; no jump lands inside it)
//
//   0x00E298BB  mov byte ptr [eax+0x3C4], cl   (6 bytes)   -> jmp slideHoldStub, nop
//
// eax is the action input block [action+0x78], cl the new latched bits (bit 0 = slide contact this frame). The stub
// asks slideHoldUpdate for the bits to store, stores them and jumps back to 0x00E298C1. xmm0-xmm2 are live at that
// point (the game stores them right after), so the stub saves and restores them around the call.

namespace {
    constexpr DWORD kImageBase = 0x00400000;
    constexpr DWORD kSite = 0x00E298BB;
    const BYTE kSiteBytes[6] = { 0x88, 0x88, 0xC4, 0x03, 0x00, 0x00 };
    constexpr DWORD kReturn = 0x00E298C1;
    // The 5 instructions before the site, checked so the registers mean what the comment above says.
    constexpr DWORD kBefore = 0x00E298AF;
    const BYTE kBeforeBytes[12] = { 0x0F, 0xB6, 0x98, 0xC4, 0x03, 0x00, 0x00, 0x80, 0xE3, 0xF8, 0x0A, 0xCB };

    constexpr float kHold = 1.0f / 30.0f;   // one 30 FPS frame
    constexpr int kSlots = 64;

    struct HoldSlot {
        DWORD block;      // the action input block, 0 = free
        float remaining;  // seconds the contact is still kept
        DWORD lastTick;   // GetTickCount of the last update
    };
    HoldSlot g_slots[kSlots];


    HoldSlot* findSlot(DWORD block, bool create, DWORD now) {
        const unsigned start = (unsigned)((((uint32_t)block >> 4) * 2654435761u) >> 26); // 0..63
        HoldSlot* freeSlot = nullptr;
        for (int n = 0; n < kSlots; ++n) {
            HoldSlot& s = g_slots[(start + n) & (kSlots - 1)];
            if (s.block == block) return &s;
            if (!freeSlot && (!s.block || now - s.lastTick > 1000)) freeSlot = &s;
        }
        if (!create || !freeSlot) return nullptr;
        freeSlot->block = block;
        freeSlot->remaining = 0.0f;
        freeSlot->lastTick = now;
        return freeSlot;
    }
}

// Game thread, once per character per frame. Returns the bits to store at [block+0x3C4].
extern "C" unsigned __cdecl slideHoldUpdate(DWORD block, unsigned bits) {
    const DWORD now = GetTickCount();
    if (bits & 1) {
        HoldSlot* s = findSlot(block, true, now);
        if (s) {
            s->remaining = kHold;
            s->lastTick = now;
        }
        return bits;
    }
    HoldSlot* s = findSlot(block, false, now);
    if (!s || !(s->remaining > 0.0f)) return bits;
    if (now - s->lastTick > 250) { s->remaining = 0.0f; return bits; } // a load or a long hitch: nothing to keep
    float step = g_PhysicsDeltaTime;
    if (!(step > 0.0f)) step = 0.0f;
    s->remaining -= step;
    s->lastTick = now;
    if (s->remaining > 0.0005f) return bits | 1;
    s->remaining = 0.0f;
    return bits;
}

volatile DWORD g_SlideHoldReturn = kReturn;

__declspec(naked) void slideHoldStub() {
    __asm {
        pushad                              // eax at +0x1C, ecx at +0x18
        sub esp, 0x30
        movups [esp + 0x00], xmm0
        movups [esp + 0x10], xmm1
        movups [esp + 0x20], xmm2
        movzx ecx, cl
        push ecx                            // bits
        push eax                            // action input block
        call slideHoldUpdate
        add esp, 8
        mov byte ptr[esp + 0x30 + 0x18], al // becomes cl after popad
        movups xmm0, [esp + 0x00]
        movups xmm1, [esp + 0x10]
        movups xmm2, [esp + 0x20]
        add esp, 0x30
        popad
        mov byte ptr[eax + 0x3C4], cl       // the instruction that was replaced
        jmp dword ptr[g_SlideHoldReturn]
    }
}

bool installSlopeSlideFix() {
    if ((DWORD)GetModuleHandle(NULL) != kImageBase) {
        SDLOG(0, "SlopeSlideFix: game image is not at its expected base 0x%08X, nothing patched\n", kImageBase);
        return false;
    }
    if (memcmp((const void*)kSite, kSiteBytes, sizeof(kSiteBytes)) != 0 ||
        memcmp((const void*)kBefore, kBeforeBytes, sizeof(kBeforeBytes)) != 0) {
        SDLOG(0, "SlopeSlideFix: the slide contact site 0x%08X does not match this build, nothing patched\n", kSite);
        return false;
    }
    memset(g_slots, 0, sizeof(g_slots));

    DWORD oldProtect;
    if (!VirtualProtect((LPVOID)kSite, sizeof(kSiteBytes), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        SDLOG(0, "SlopeSlideFix: could not patch 0x%08X (error %lu)\n", kSite, GetLastError());
        return false;
    }
    BYTE* p = (BYTE*)kSite;
    const DWORD rel = (DWORD)&slideHoldStub - (kSite + 5);
    p[0] = 0xE9;
    memcpy(p + 1, &rel, sizeof(rel));
    p[5] = 0x90;
    VirtualProtect((LPVOID)kSite, sizeof(kSiteBytes), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)kSite, sizeof(kSiteBytes));

    SDLOG(0, "SlopeSlideFix: installed (slide contact kept for 1/30 s at 0x%08X)\n", kSite);
    return true;
}
