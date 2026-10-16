#include "StaminaFix.h"
#include "main.h" // For SDLOG
#include <windows.h>
#include <cstring>

// Sprint stamina tick.
// The character update (fn 0xE80A00) keeps a timer at [chr+0x168]: while the character spends stamina it adds the
// frame time, and when the sum reaches the interval (0.1 s, the float at 0x12DF014) it takes one point off the
// stamina and then STORES ZERO in the timer (movss [ebx+168h], xmm2 at 0xE80B00, xmm2 being 0). The leftover of the
// last frame is thrown away, so each tick lasts 0.1 s rounded up to whole frames: at awkward frame rates the drain
// runs up to a frame per tick slow. That store becomes a call that subtracts the interval instead, which makes the
// drain exactly 10 points a second at any frame rate. (DSfix never writes a step longer than 0.1 s, so the timer
// cannot build up a growing remainder.)

namespace {
    constexpr DWORD kTickStoreSite = 0x00E80B00;
    constexpr DWORD kTickInterval = 0x012DF014;
    const BYTE kExpect[8] = { 0xF3, 0x0F, 0x11, 0x93, 0x68, 0x01, 0x00, 0x00 }; // movss [ebx+168h], xmm2
}

__declspec(naked) void stamina_tick_stub() {
    __asm {
        pushfd                          // Save flags (the jg after the call needs them)
        fld dword ptr[ebx + 0x168]     // Load the current stamina timer into the FPU stack
        fsub dword ptr ds : [0x012DF014]  // Subtract exactly the tick interval (0.1 s)
        fstp dword ptr[ebx + 0x168]    // Store the remainder back into the timer
        popfd                           // Restore flags
        ret                             // Return to the game execution
    }
}

bool installStaminaFix() {
    BYTE* site = (BYTE*)kTickStoreSite;

    // Safety checks: the exact original instruction, and a sane interval constant
    if (memcmp(site, kExpect, sizeof(kExpect)) != 0) {
        SDLOG(0, "StaminaFix: Memory mismatch at 0x%08X, skipping sprint stamina fix.\n", kTickStoreSite);
        return false;
    }
    const float interval = *(const float*)kTickInterval;
    if (!(interval >= 0.05f && interval <= 0.5f)) {
        SDLOG(0, "StaminaFix: tick interval is %.4f, not what was expected, skipping sprint stamina fix.\n", interval);
        return false;
    }

    DWORD oldProtect;
    if (VirtualProtect(site, 8, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        // We replace the 8-byte instruction with a CALL (5 bytes) + 3 NOPs
        site[0] = 0xE8; // CALL opcode
        *(DWORD*)(site + 1) = (DWORD)stamina_tick_stub - (kTickStoreSite + 5);
        site[5] = 0x90; // NOP
        site[6] = 0x90; // NOP
        site[7] = 0x90; // NOP

        VirtualProtect(site, 8, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), site, 8);

        SDLOG(0, "StaminaFix: Successfully patched sprint stamina drain (interval %.3f s).\n", interval);
        return true;
    }
    return false;
}
