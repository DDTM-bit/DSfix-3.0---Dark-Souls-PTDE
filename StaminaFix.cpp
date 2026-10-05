#include "StaminaFix.h"
#include "main.h" // For SDLOG
#include <windows.h>

__declspec(naked) void stamina_tick_stub() {
    __asm {
        pushfd                          // Save flags
        fld dword ptr[ebx + 0x168]     // Load the current stamina timer into the FPU stack
        fsub dword ptr ds : [0x012DF014]  // Subtract exactly 0.1s (the constant stored here)
        fstp dword ptr[ebx + 0x168]    // Store the remainder back into the timer
        popfd                           // Restore flags
        ret                             // Return to the game execution
    }
}

bool installStaminaFix() {
    // Location of the instruction: movss dword ptr [ebx+168h], xmm2 (where xmm2 is 0)
    DWORD kTickStoreSite = 0x00E80B00;

    BYTE* site = (BYTE*)kTickStoreSite;

    // Safety Check: F3 0F 11 93 68 01 00 00
    if (site[0] != 0xF3 || site[1] != 0x0F || site[2] != 0x11) {
        SDLOG(0, "StaminaFix: Memory mismatch, skipping sprint stamina fix.\n");
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

        SDLOG(0, "StaminaFix: Successfully patched sprint stamina drain.\n");
        return true;
    }
    return false;
}