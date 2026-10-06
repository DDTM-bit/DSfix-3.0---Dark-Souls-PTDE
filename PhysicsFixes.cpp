#include "PhysicsFixes.h"
#include "main.h"
#include <cmath>
#include <cstring>

float g_PhysicsDeltaTime = 1.0f / 30.0f;

//ghost bloodstain
float g_GhostUnitsPerSecond = 30.0f * 65536.0f;
int32_t g_GhostRecStep = -65536;
float g_GhostTurnScale = 1.0f;
__declspec(align(16)) float g_GhostStepScale[4] = { 1.0f, 1.0f, 1.0f, 1.0f };


namespace {
    // 1. Slide Gravity & Damping
    alignas(4) volatile float g_scaledGravity = 1.0f;
    alignas(4) volatile float g_scaledFriction = 0.65f;
    alignas(8) volatile double g_scaledDamping = 0.95;

    // 2. Sprint Graze Check
    alignas(8) volatile double g_scaledRate = 30.0;
    alignas(8) volatile double g_scaledSlow = 0.8;
    alignas(8) volatile double g_scaledRecover = 1.2;

    // 3. Velocity (Ragdolls & 3D Audio)
    alignas(8) volatile double g_invDeltaTime = 30.0;

    // 4. Timers and Fades
    alignas(8) volatile double g_scaledTimerDt = 1.0 / 30.0;
    alignas(8) volatile double g_scaledSmooth = 1.0 / 30.0;
    alignas(8) volatile double g_scaledShineDiv = 60.0;

    // 5. Menu Input Repeat (Locked to 30 FPS timing)
    alignas(4) volatile float g_fixedStep = 1.0f / 30.0f;

    struct PatchSite {
        DWORD instructionAddress;
        DWORD operandOffset;
        DWORD originalTarget;
        const volatile void* newTarget;
    };

    PatchSite g_sites[] = {
        // --- Slide Gravity & Airborne Damping ---
        { 0x00EC0F8D, 4, 0x012DF970, &g_scaledGravity },
        { 0x00EC13A6, 4, 0x012DF974, &g_scaledFriction },
        { 0x00EC184F, 4, 0x011E8388, &g_scaledDamping },

        // --- Sprint Graze Slowdown Fixes ---
        { 0x00E3ABA2, 4, 0x011E7CD8, &g_scaledRate },
        { 0x00E3ABCB, 4, 0x011E7DE8, &g_scaledSlow },
        { 0x00E3AC01, 4, 0x011E84F8, &g_scaledRecover },

        // --- Velocity Fixes (Ragdolls & 3D Audio) ---
        { 0x00DEB9BF, 4, 0x011E7CD8, &g_invDeltaTime },
        { 0x00DF1809, 4, 0x011E7CD8, &g_invDeltaTime },
        { 0x00EC6562, 2, 0x011E7CD8, &g_invDeltaTime },

        // --- Timers, Fades, and Camera Smoothing ---
        { 0x00DF2C55, 4, 0x011E7CF0, &g_scaledTimerDt },  // Fade timer += 1/30
        { 0x00DF2C7A, 4, 0x011E7CF0, &g_scaledTimerDt },  // Fade timer -= 1/30
        { 0x00CFB2B6, 4, 0x011E7CF0, &g_scaledTimerDt },  // Countdown -= 1/30
        { 0x00EC7769, 4, 0x011E7CF0, &g_scaledTimerDt },  // Timer + 1/30
        { 0x00F9179D, 4, 0x011E7CF0, &g_scaledTimerDt },  // Countdown step
        { 0x00F0B556, 4, 0x011E7CF0, &g_scaledSmooth },   // Lerp factor
        { 0x00E853BE, 4, 0x011E7FF8, &g_scaledShineDiv }, // Item glow fade 1
        { 0x00E85525, 4, 0x011E7FF8, &g_scaledShineDiv }, // Item glow fade 2

        // --- Menu Input Repeat & Hardware Config (Fixed 1/30) ---
        { 0x0087B526, 2, 0x011E7E90, &g_fixedStep }, // Havok World Setup
        { 0x00D8621C, 4, 0x011E7E90, &g_fixedStep }, // ImageFilter
        { 0x00F7C78E, 4, 0x011E7E90, &g_fixedStep }, // Input Repeat 1
        { 0x00F7C7C6, 4, 0x011E7E90, &g_fixedStep }, // Input Repeat 2
        { 0x00F7C7FE, 4, 0x011E7E90, &g_fixedStep }, // Input Repeat 3
        { 0x00F7C836, 4, 0x011E7E90, &g_fixedStep }, // Input Repeat 4
        { 0x00F7C86E, 4, 0x011E7E90, &g_fixedStep }, // Input Repeat 5
        { 0x00F7C8A6, 4, 0x011E7E90, &g_fixedStep }, // Input Repeat 6
        { 0x00F7C8DE, 4, 0x011E7E90, &g_fixedStep }, // Input Repeat 7
        { 0x00F7C916, 4, 0x011E7E90, &g_fixedStep }  // Input Repeat 8
    };
}

bool installPhysicsFixes() {
    for (const auto& site : g_sites) {
        DWORD targetAddr = site.instructionAddress + site.operandOffset;
        DWORD currentTarget = 0;

        memcpy(&currentTarget, (const void*)targetAddr, sizeof(DWORD));
        if (currentTarget != site.originalTarget) {
            SDLOG(0, "PhysicsFixes: Address mismatch at 0x%08X (expected 0x%08X)\n",
                site.instructionAddress, site.originalTarget);
            return false;
        }

        DWORD oldProtect;
        if (VirtualProtect((LPVOID)targetAddr, sizeof(DWORD), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            DWORD newAddr = (DWORD)site.newTarget;
            memcpy((void*)targetAddr, &newAddr, sizeof(DWORD));
            VirtualProtect((LPVOID)targetAddr, sizeof(DWORD), oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), (LPCVOID)site.instructionAddress, 8);
        }
        else {
            return false;
        }
    }

    SDLOG(0, "PhysicsFixes: All physics, velocity, timer, and menu input checks patched.\n");
    return true;
}

void updatePhysicsDeltas(float deltaSeconds) {
    if (deltaSeconds < 0.001f) deltaSeconds = 0.001f;
    if (deltaSeconds > 0.125f) deltaSeconds = 0.125f;

    g_PhysicsDeltaTime = deltaSeconds;
    double s = (double)deltaSeconds * 30.0;

    // Slide Gravity & Damping
    g_scaledGravity = (float)(1.0 * s);
    g_scaledFriction = (float)std::pow(0.65, s);
    g_scaledDamping = std::pow(0.95, s);

    // Graze Check Scaling
    g_scaledRate = 1.0 / (double)deltaSeconds;
    g_scaledSlow = std::pow(0.8, s);
    g_scaledRecover = std::pow(1.2, s);

    // Velocity Scaling (Ragdolls & Audio)
    g_invDeltaTime = 1.0 / (double)deltaSeconds;

    // Timers and Fades
    g_scaledTimerDt = (double)deltaSeconds;
    g_scaledSmooth = 1.0 - std::pow(1.0 - (1.0 / 30.0), s);
    g_scaledShineDiv = 2.0 / (double)deltaSeconds;

    // Ghost Fixed-Point Timers & Translation Scaling (v1.5.0 logic)
    g_GhostRecStep = -(int32_t)(s * 65536.0);
    g_GhostTurnScale = (float)s;
    g_GhostStepScale[0] = g_GhostStepScale[1] = g_GhostStepScale[2] = g_GhostStepScale[3] = (float)s;
}