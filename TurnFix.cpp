#include "TurnFix.h"
#include "PhysicsFixes.h" // For g_PhysicsDeltaTime
#include "main.h"         // For SDLOG
#include <windows.h>
#include <cmath>
#include <cstdint>

namespace {
    // Typedef for the original bone controller update function
    typedef void* (__stdcall* BoneFn)(void* controller, void* out, void* pose);
    BoneFn g_bone_orig = nullptr;

    // Our detour function
    void* __stdcall hk_bone(void* controller, void* out, void* pose) {
        // The blend weight (gain) is stored at offset +0x2C in the controller object
        float* gain = (float*)((uint8_t*)controller + 0x2C);
        float saved = *gain;

        // Only scale if it's a valid fractional blend weight
        if (saved > 0.0f && saved < 1.0f) {
            double n = (double)g_PhysicsDeltaTime * 30.0;
            if (n < 0.01) n = 0.01;
            if (n > 4.0) n = 4.0;

            // Apply the framerate-independent decay formula
            *gain = (float)(1.0 - std::pow(1.0 - (double)saved, n));

            // Run the original rotation logic with the scaled weight
            void* result = g_bone_orig(controller, out, pose);

            // Restore the original weight so the struct isn't permanently altered
            *gain = saved;
            return result;
        }

        // If not a fractional weight, just run normally
        return g_bone_orig(controller, out, pose);
    }
}

bool installTurnFix() {
    DWORD kBoneUpdate = 0x00D901D0;
    BYTE* site = (BYTE*)kBoneUpdate;

    // Safety Check: push ebp ; mov ebp,esp ; and esp,-16
    if (site[0] != 0x55 || site[1] != 0x8B || site[2] != 0xEC || site[3] != 0x83) {
        SDLOG(0, "TurnFix: Memory mismatch, skipping lock-on turn fix.\n");
        return false;
    }

    // Allocate a memory cave for the trampoline
    BYTE* tramp = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;

    // Copy the first 6 stolen bytes to the trampoline
    memcpy(tramp, site, 6);

    // Add a JMP back to the original function after the stolen bytes
    tramp[6] = 0xE9;
    *(DWORD*)(tramp + 7) = (kBoneUpdate + 6) - ((DWORD)tramp + 11);

    g_bone_orig = (BoneFn)tramp;

    DWORD oldProtect;
    if (VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        // Overwrite the original function start with a JMP to our hook
        site[0] = 0xE9;
        *(DWORD*)(site + 1) = (DWORD)hk_bone - (kBoneUpdate + 5);
        site[5] = 0x90; // NOP the leftover 6th byte to keep assembly aligned

        VirtualProtect(site, 6, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), site, 6);

        SDLOG(0, "TurnFix: Lock-on body turn successfully patched.\n");
        return true;
    }
    return false;
}