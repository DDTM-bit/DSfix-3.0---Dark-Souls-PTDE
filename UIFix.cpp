#include "UIFix.h"
#include "PhysicsFixes.h" // For g_PhysicsDeltaTime
#include "main.h"
#include <windows.h>
#include <cstdint>

extern float g_PhysicsDeltaTime;

namespace {
    // --- Loading Screen Swirl ---
    double g_swirl_carry = 0.0;

    uint32_t __stdcall swirl_steps() {
        double elapsed = g_PhysicsDeltaTime;
        // Prevent massive jumps during load hitches
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

    // 1. Patch Loading Swirl (0x00C383FD)
    DWORD kSwirlSite = 0x00C383FD;
    if (*(BYTE*)kSwirlSite == 0xB8) { // Safety check: mov eax, 1
        if (VirtualProtect((LPVOID)kSwirlSite, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            *(BYTE*)kSwirlSite = 0xE8; // call
            *(DWORD*)(kSwirlSite + 1) = (DWORD)swirl_stub - (kSwirlSite + 5);
            VirtualProtect((LPVOID)kSwirlSite, 5, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), (LPCVOID)kSwirlSite, 5);
        }
    }
    else return false;

    // 2. Patch HUD Gauge Follower (0x00C88BA0)
    DWORD kGaugeFollow = 0x00C88BA0;
    if (*(BYTE*)kGaugeFollow == 0xF3) { // Safety check: movss
        BYTE* tramp = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!tramp) return false;

        memcpy(tramp, (void*)kGaugeFollow, 5);
        tramp[5] = 0xE9; // jmp back
        *(DWORD*)(tramp + 6) = (kGaugeFollow + 5) - ((DWORD)tramp + 10);
        g_gauge_follow_tramp = tramp;

        if (VirtualProtect((LPVOID)kGaugeFollow, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            *(BYTE*)kGaugeFollow = 0xE9; // jmp
            *(DWORD*)(kGaugeFollow + 1) = (DWORD)gauge_follow_hook - (kGaugeFollow + 5);
            VirtualProtect((LPVOID)kGaugeFollow, 5, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), (LPCVOID)kGaugeFollow, 5);
        }
    }
    else return false;

    SDLOG(0, "UIFix: Swirl and gauge animations successfully patched.\n");
    return true;
}