#include "TurnFix.h"
#include "PhysicsFixes.h" // For g_PhysicsDeltaTime
#include "main.h"         // For SDLOG
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>

// Lock-on body turn (original research: zofrea, turn.cpp).
// With a target locked, TurnAnim and WalkAnim_Twist turn the Upper_Root, Lower_Root, Spine, Spine1 and Head bones
// toward it through bone rotation controllers. Each controller's update (fn 0xD901D0, stdcall(controller, out, pose))
// moves its current angle toward the target by a per-frame blend weight [+0x2C] (0.6 roots, 0.2 head, 0.1 spine...)
// with no frame time. For the duration of each update the weight is replaced by 1 - (1 - w) ^ (step * 30).

namespace {
    constexpr DWORD kBoneUpdate = 0x00D901D0;
    constexpr size_t kStolen = 6;
    // push ebp ; mov ebp,esp ; and esp,-16 ; xorps xmm0,xmm0
    const BYTE kExpect[9] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x0F, 0x57, 0xC0 };

    // Typedef for the original bone controller update function
    typedef void* (__stdcall* BoneFn)(void* controller, void* out, void* pose);
    BoneFn g_bone_orig = nullptr;

    // Performance: this runs for every turning bone of every character, every frame, but only a handful of distinct
    // weights exist. A tiny cache keyed on (weight, n) avoids almost every pow() call. Single game thread.
    struct GainCache { float w; float n; float result; };
    GainCache g_cache[8] = {};

    float scaled_gain(float w, float n) {
        uint32_t bits;
        memcpy(&bits, &w, 4);
        GainCache& c = g_cache[(bits >> 12) & 7];
        if (c.w == w && c.n == n) return c.result;
        c.w = w;
        c.n = n;
        c.result = (float)(1.0 - std::pow(1.0 - (double)w, (double)n));
        return c.result;
    }

    // Our detour function
    void* __stdcall hk_bone(void* controller, void* out, void* pose) {
        // The blend weight (gain) is stored at offset +0x2C in the controller object
        float* gain = (float*)((uint8_t*)controller + 0x2C);
        const float saved = *gain;

        // Only scale if it's a valid fractional blend weight
        if (saved > 0.0f && saved < 1.0f) {
            float n = g_PhysicsDeltaTime * 30.0f; // the engine's own step (see FPS.cpp)
            if (n < 0.01f) n = 0.01f;
            if (n > 4.0f) n = 4.0f;

            *gain = scaled_gain(saved, n);
            void* result = g_bone_orig(controller, out, pose);
            *gain = saved; // Restore the original weight so the struct isn't permanently altered
            return result;
        }

        // If not a fractional weight, just run normally
        return g_bone_orig(controller, out, pose);
    }
}

bool installTurnFix() {
    BYTE* site = (BYTE*)kBoneUpdate;

    // Safety check (as in zofrea): the exact original prologue
    if (memcmp(site, kExpect, sizeof(kExpect)) != 0) {
        SDLOG(0, "TurnFix: Memory mismatch at 0x%08X, skipping lock-on turn fix.\n", kBoneUpdate);
        return false;
    }

    // Allocate a memory cave for the trampoline
    BYTE* tramp = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;

    // Copy the first 6 stolen bytes to the trampoline, then JMP back to the original function after them
    memcpy(tramp, site, kStolen);
    tramp[kStolen] = 0xE9;
    *(DWORD*)(tramp + kStolen + 1) = (kBoneUpdate + kStolen) - ((DWORD)tramp + kStolen + 5);
    FlushInstructionCache(GetCurrentProcess(), tramp, 32);

    g_bone_orig = (BoneFn)tramp;

    DWORD oldProtect;
    if (VirtualProtect(site, kStolen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        // Overwrite the original function start with a JMP to our hook
        site[0] = 0xE9;
        *(DWORD*)(site + 1) = (DWORD)hk_bone - (kBoneUpdate + 5);
        site[5] = 0x90; // NOP the leftover 6th byte

        VirtualProtect(site, kStolen, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), site, kStolen);

        SDLOG(0, "TurnFix: Lock-on body turn successfully patched.\n");
        return true;
    }
    return false;
}
