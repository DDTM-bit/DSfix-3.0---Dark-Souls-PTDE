
#include "UIFix.h"
#include "PhysicsFixes.h" // For g_PhysicsDeltaTime
#include "main.h"
#include <windows.h>
#include <cstdint>
#include <cstring>

extern float g_PhysicsDeltaTime;

namespace {
    // --- Loading Screen Swirl ---
    // FrpgMenuDlgNowLoading::update (0xC383C0) advances two counters by `mov eax, 1` every time it is drawn; at
    // 30 FPS that is 30 steps a second. The mov becomes a call returning how many 1/30 s steps have REALLY elapsed
    // since the last call (measured with its own clock, because the swirl is not drawn every
    // frame and the engine step is clamped, so the frame step would make it run slow during loading hitches).
    double g_swirl_carry = 0.0;
    LARGE_INTEGER g_swirl_freq = {};
    LARGE_INTEGER g_swirl_last = {};

    uint32_t __stdcall swirl_steps() {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (g_swirl_freq.QuadPart == 0) QueryPerformanceFrequency(&g_swirl_freq);
        if (g_swirl_last.QuadPart == 0) {
            g_swirl_last = now;
            return 1;
        }
        double elapsed = (double)(now.QuadPart - g_swirl_last.QuadPart) / (double)g_swirl_freq.QuadPart;
        g_swirl_last = now;
        // The swirl is not drawn while the screen is being built; a long gap must not skip ahead
        if (elapsed > 0.25) elapsed = 1.0 / 30.0;

        g_swirl_carry += elapsed * 30.0;
        uint32_t whole = (uint32_t)g_swirl_carry;
        g_swirl_carry -= whole;
        return whole;
    }

    __declspec(naked) void swirl_stub() {
        __asm {
            push ecx
            push edx
            call swirl_steps
            pop edx
            pop ecx
            ret
        }
    }

    // --- HUD Gauge Animation ---
    void* g_gauge_follow_tramp = nullptr;
    float g_gauge_saved[3] = {};

    void __stdcall gauge_scale_enter(uint8_t* gauge) {
        float s = g_PhysicsDeltaTime * 30.0f;
        if (s < 0.02f) s = 0.02f;
        if (s > 4.0f) s = 4.0f;

        // Gauge fill rates are stored at +0x44, +0x48, and +0x4C
        float* steps[3] = { (float*)(gauge + 0x44), (float*)(gauge + 0x48), (float*)(gauge + 0x4C) };
        for (int i = 0; i < 3; ++i) {
            g_gauge_saved[i] = *steps[i];
            *steps[i] = g_gauge_saved[i] * s;
        }

        // Golden trail on small changes (stamina while running at 60+ FPS).
        // Gauge layout (FrpgMenuDlgObjSmoothGauge; checked in the exe: constructor 0x00C88F30, follower 0x00C88BA0,
        // update 0x00C88FD0): +0x20 = value the bar must show (0..1), +0x3C = value it shows, +0x44 = step down
        // (0.012 by default), +0x4C = step up, +0x48 = step of the second value (+0x40 toward +0x24).
        // +0x50 = "moving": set by the follower when a change is bigger than the step. +0x51 = "may move": set by the
        // update once the hold is over; until then the follower does not move the shown value at all (0x00C88C7F,
        // 0x00C88CCA). +0x54 = hold timer (the update adds the frame time), +0x5C = hold length (1.5 s by default; the
        // menu data can give a gauge its own steps and hold). A change not bigger than the step is shown at once.
        // So the yellow part is the old value held for the hold length, then drained step by step.
        // Sprinting takes 1 stamina point = 1/160 = 0.00625 every 0.1 s: at 30 FPS the step 0.012 is bigger, so the
        // bar snaps. At 60 FPS the scaled step is 0.006, smaller than one point, so every point started a hold and
        // the yellow part came back again and again. When the gap fits in the unscaled (30 FPS) step, the follower
        // gets that step: it snaps exactly as it does at 30 FPS. Bigger gaps (real damage) keep the scaled step, so
        // the yellow part of a hit still drains at the same speed at any frame rate.
        const float target = *(float*)(gauge + 0x20);
        const float shown = *(float*)(gauge + 0x3C);
        const float down = shown - target, up = target - shown;
        if (down > 0.0f && down <= g_gauge_saved[0]) *steps[0] = g_gauge_saved[0];
        if (up > 0.0f && up <= g_gauge_saved[2]) *steps[2] = g_gauge_saved[2];
    }

    void __stdcall gauge_scale_leave(uint8_t* gauge) {
        // Restore the original rates so the UI doesn't permanently break
        *(float*)(gauge + 0x44) = g_gauge_saved[0];
        *(float*)(gauge + 0x48) = g_gauge_saved[1];
        *(float*)(gauge + 0x4C) = g_gauge_saved[2];
    }

    // The follower gets its gauge in eax. ecx, edx and xmm0-xmm7 are saved around our own C++ code, so it cannot
    // change any register the original function reads or returns.
    __declspec(naked) void gauge_follow_hook() {
        __asm {
            push eax                        // [esp] = gauge (kept for the leave call)
            push ecx
            push edx
            sub esp, 128
            movdqu [esp + 0], xmm0
            movdqu [esp + 16], xmm1
            movdqu [esp + 32], xmm2
            movdqu [esp + 48], xmm3
            movdqu [esp + 64], xmm4
            movdqu [esp + 80], xmm5
            movdqu [esp + 96], xmm6
            movdqu [esp + 112], xmm7
            push eax
            call gauge_scale_enter
            movdqu xmm0, [esp + 0]
            movdqu xmm1, [esp + 16]
            movdqu xmm2, [esp + 32]
            movdqu xmm3, [esp + 48]
            movdqu xmm4, [esp + 64]
            movdqu xmm5, [esp + 80]
            movdqu xmm6, [esp + 96]
            movdqu xmm7, [esp + 112]
            add esp, 128
            pop edx
            pop ecx
            mov eax, dword ptr[esp]
            call dword ptr[g_gauge_follow_tramp]
            push eax                        // results of the original function
            push ecx
            push edx
            sub esp, 32
            movdqu [esp + 0], xmm0
            movdqu [esp + 16], xmm1
            push dword ptr[esp + 44]        // the gauge saved at entry
            call gauge_scale_leave
            movdqu xmm0, [esp + 0]
            movdqu xmm1, [esp + 16]
            add esp, 32
            pop edx
            pop ecx
            pop eax
            add esp, 4                      // drop the saved gauge
            ret
        }
    }
}

bool installUIFix() {
    DWORD oldProtect;
    bool swirlOk = false, gaugeOk = false;

    // 1. Patch Loading Swirl (0x00C383FD): exact `mov eax, 1`
    const DWORD kSwirlSite = 0x00C383FD;
    static const BYTE kSwirlExpect[5] = { 0xB8, 0x01, 0x00, 0x00, 0x00 };
    if (memcmp((const void*)kSwirlSite, kSwirlExpect, sizeof(kSwirlExpect)) == 0) {
        if (VirtualProtect((LPVOID)kSwirlSite, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            *(BYTE*)kSwirlSite = 0xE8; // call
            *(DWORD*)(kSwirlSite + 1) = (DWORD)swirl_stub - (kSwirlSite + 5);
            VirtualProtect((LPVOID)kSwirlSite, 5, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), (LPCVOID)kSwirlSite, 5);
            swirlOk = true;
        }
    }
    else SDLOG(0, "UIFix: loading swirl site 0x%08X does not match this build, swirl not patched.\n", kSwirlSite);

    // 2. Patch HUD Gauge Follower (0x00C88BA0): exact `movss xmm3, [eax+44h]`.
    //    Independent of the swirl: one failing no longer disables the other.
    const DWORD kGaugeFollow = 0x00C88BA0;
    static const BYTE kGaugeExpect[5] = { 0xF3, 0x0F, 0x10, 0x58, 0x44 };
    if (memcmp((const void*)kGaugeFollow, kGaugeExpect, sizeof(kGaugeExpect)) == 0) {
        BYTE* tramp = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (tramp) {
            memcpy(tramp, (void*)kGaugeFollow, 5);
            tramp[5] = 0xE9; // jmp back
            *(DWORD*)(tramp + 6) = (kGaugeFollow + 5) - ((DWORD)tramp + 10);
            FlushInstructionCache(GetCurrentProcess(), tramp, 32);
            g_gauge_follow_tramp = tramp;

            if (VirtualProtect((LPVOID)kGaugeFollow, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                *(BYTE*)kGaugeFollow = 0xE9; // jmp
                *(DWORD*)(kGaugeFollow + 1) = (DWORD)gauge_follow_hook - (kGaugeFollow + 5);
                VirtualProtect((LPVOID)kGaugeFollow, 5, oldProtect, &oldProtect);
                FlushInstructionCache(GetCurrentProcess(), (LPCVOID)kGaugeFollow, 5);
                gaugeOk = true;
            }
        }
    }
    else SDLOG(0, "UIFix: gauge follower at 0x%08X does not match this build, gauge not patched.\n", kGaugeFollow);

    SDLOG(0, "UIFix: loading swirl %s, HUD gauge follower %s.\n", swirlOk ? "patched" : "SKIPPED", gaugeOk ? "patched" : "SKIPPED");
    return swirlOk && gaugeOk;
}


