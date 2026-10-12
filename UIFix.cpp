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
    // since the last call (zofrea's approach: measured with its own clock, because the swirl is not drawn every
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
    }

    void __stdcall gauge_scale_leave(uint8_t* gauge) {
        // Restore the original rates so the UI doesn't permanently break
        *(float*)(gauge + 0x44) = g_gauge_saved[0];
        *(float*)(gauge + 0x48) = g_gauge_saved[1];
        *(float*)(gauge + 0x4C) = g_gauge_saved[2];
    }

    __declspec(naked) void gauge_follow_hook() {
        __asm {
            push eax
            push eax
            call gauge_scale_enter
            mov eax, dword ptr[esp]
            call dword ptr[g_gauge_follow_tramp]
            push eax
            call gauge_scale_leave
            pop eax
            ret
        }
    }
}

bool installUIFix() {
    DWORD oldProtect;
    bool swirlOk = false, gaugeOk = false;

    // 1. Patch Loading Swirl (0x00C383FD): exact `mov eax, 1` (as in zofrea)
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

    // 2. Patch HUD Gauge Follower (0x00C88BA0): exact `movss xmm3, [eax+44h]` (as in zofrea).
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
