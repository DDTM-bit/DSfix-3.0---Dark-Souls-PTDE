
#include "PhysicsFixes.h"
#include "main.h"
#include <cmath>
#include <cstring>
#include <cstdint>

// Per-frame constants in the game's code that were tuned for a fixed 30 FPS, made frame-rate independent.
// Each site retargets the 32-bit address operand of one load so it reads a value
// recomputed every frame from the game's own time step:
//
//   slide gravity   1.0  per frame         -> 1.0 * s            (s = step * 30)
//   slide friction  0.65 per frame         -> 0.65 ^ s
//   air damping     0.95 per frame         -> 0.95 ^ s
//   graze check     |delta| * 30 < 1       -> |delta| / step < 1;  0.8 / 1.2 per frame -> ^ s
//   velocities      delta * 30             -> delta / step
//   timers          +/- 1/30 per frame     -> step
//   smoothing       lerp factor 1/30       -> 1 - (1 - 1/30) ^ s
//   item glow fade  [+0x18] / 60 per frame -> [+0x18] * step / 2
//   copied step     1/30 copied into settings -> fixed 1/30
//   HUD gauges      |gap| * 0.5 and 1.0 per frame -> 1 - 0.5 ^ s and 1.0 * s
//
// At exactly 30 FPS every value equals the game's original constant.
//
// DSfix 3.0: the step passed to updatePhysicsDeltas() is the exact step DSfix writes into the engine
// (see FPS.cpp), so physics and game time can never drift apart (hitches, frame-limit jitter, pause).

float g_PhysicsDeltaTime = 1.0f / 30.0f;

//ghost bloodstain
float g_GhostUnitsPerSecond = 30.0f * 65536.0f;
int32_t g_GhostRecStep = -65536;
float g_GhostTurnScale = 1.0f;
__declspec(align(16)) float g_GhostStepScale[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
// Other players' walk / run / sprint (GhostFix.cpp): frames per second of this frame, 30 at 30 FPS
__declspec(align(8)) double g_GhostStepsPerSecond = 30.0;


namespace {
    // The game is a fixed-base 32-bit image; all addresses below are absolute.
    constexpr DWORD kImageBase = 0x00400000;

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

    // 6. HUD gauges (FrpgMenuDlgObjGauge update, fn 0xC89A40: HP, stamina, boss and enemy bars): the displayed value
    //    chases the real one by |gap| * 0.5 per frame while the gap is over 2, else by 1.0 per frame.
    alignas(8) volatile double g_gaugeK = 0.5;
    alignas(4) volatile float g_gaugeMin = 1.0f;

    // Groups are installed all-or-nothing: if one site of a group does not match this build, the whole group is
    // left untouched, so e.g. the graze check never ends up half scaled.
    enum Group { G_SLIDE, G_DAMPING, G_GRAZE, G_VELOCITY, G_TIMERS, G_FIXED_STEP, G_GAUGE, G_COUNT };
    const char* const kGroupNames[G_COUNT] = { "slide", "damping", "graze", "velocity", "timers", "fixed step", "hud gauges" };

    struct PatchSite {
        DWORD instructionAddress;
        DWORD operandOffset;
        DWORD originalTarget;
        const volatile void* newTarget;
        int group;
    };

    PatchSite g_sites[] = {
        // --- Slide Gravity & Airborne Damping ---
        { 0x00EC0F8D, 4, 0x012DF970, &g_scaledGravity,  G_SLIDE },   // movss xmm0,[1.0]   slope gravity per frame
        { 0x00EC13A6, 4, 0x012DF974, &g_scaledFriction, G_SLIDE },   // movss xmm0,[0.65]  ground friction per frame
        { 0x00EC184F, 4, 0x011E8388, &g_scaledDamping,  G_DAMPING }, // movsd xmm2,[0.95]  airborne damping per frame

        // --- Sprint Graze Slowdown Fixes ---
        { 0x00E3ABA2, 4, 0x011E7CD8, &g_scaledRate,    G_GRAZE },    // mulsd xmm0,[30.0]  -> 1 / step
        { 0x00E3ABCB, 4, 0x011E7DE8, &g_scaledSlow,    G_GRAZE },    // mulsd xmm0,[0.8]   -> 0.8 ^ s
        { 0x00E3AC01, 4, 0x011E84F8, &g_scaledRecover, G_GRAZE },    // mulsd xmm0,[1.2]   -> 1.2 ^ s

        // --- Velocity Fixes (Ragdolls & 3D Audio) ---
        { 0x00DEB9BF, 4, 0x011E7CD8, &g_invDeltaTime, G_VELOCITY },
        { 0x00DF1809, 4, 0x011E7CD8, &g_invDeltaTime, G_VELOCITY },
        { 0x00EC6562, 2, 0x011E7CD8, &g_invDeltaTime, G_VELOCITY },

        // --- Timers, Fades, and Camera Smoothing ---
        { 0x00DF2C55, 4, 0x011E7CF0, &g_scaledTimerDt,  G_TIMERS },  // Fade timer += 1/30
        { 0x00DF2C7A, 4, 0x011E7CF0, &g_scaledTimerDt,  G_TIMERS },  // Fade timer -= 1/30
        { 0x00CFB2B6, 4, 0x011E7CF0, &g_scaledTimerDt,  G_TIMERS },  // Countdown -= 1/30
        { 0x00EC7769, 4, 0x011E7CF0, &g_scaledTimerDt,  G_TIMERS },  // Timer + 1/30
        { 0x00F9179D, 4, 0x011E7CF0, &g_scaledTimerDt,  G_TIMERS },  // Countdown step
        { 0x00F0B556, 4, 0x011E7CF0, &g_scaledSmooth,   G_TIMERS },  // Lerp factor
        { 0x00E853BE, 4, 0x011E7FF8, &g_scaledShineDiv, G_TIMERS },  // Item glow fade 1
        { 0x00E85525, 4, 0x011E7FF8, &g_scaledShineDiv, G_TIMERS },  // Item glow fade 2

        // --- Menu Input Repeat & Hardware Config (Fixed 1/30) ---
        { 0x0087B526, 2, 0x011E7E90, &g_fixedStep, G_FIXED_STEP }, // Havok World Setup
        { 0x00D8621C, 4, 0x011E7E90, &g_fixedStep, G_FIXED_STEP }, // ImageFilter
        { 0x00F7C78E, 4, 0x011E7E90, &g_fixedStep, G_FIXED_STEP }, // Input Repeat 1
        { 0x00F7C7C6, 4, 0x011E7E90, &g_fixedStep, G_FIXED_STEP }, // Input Repeat 2
        { 0x00F7C7FE, 4, 0x011E7E90, &g_fixedStep, G_FIXED_STEP }, // Input Repeat 3
        { 0x00F7C836, 4, 0x011E7E90, &g_fixedStep, G_FIXED_STEP }, // Input Repeat 4
        { 0x00F7C86E, 4, 0x011E7E90, &g_fixedStep, G_FIXED_STEP }, // Input Repeat 5
        { 0x00F7C8A6, 4, 0x011E7E90, &g_fixedStep, G_FIXED_STEP }, // Input Repeat 6
        { 0x00F7C8DE, 4, 0x011E7E90, &g_fixedStep, G_FIXED_STEP }, // Input Repeat 7
        { 0x00F7C916, 4, 0x011E7E90, &g_fixedStep, G_FIXED_STEP }, // Input Repeat 8

        // --- HUD gauges: boss / enemy bars and the HP/stamina gauge objects ---
        { 0x00C89A90, 4, 0x0115F59C, &g_gaugeMin, G_GAUGE },   // movss xmm2,[1.0]  -> 1.0 * s
        { 0x00C89AC3, 4, 0x011E7C60, &g_gaugeK,   G_GAUGE }    // mulsd xmm2,[0.5]  -> 1 - 0.5 ^ s
    };

    // ln() of the per-frame factors, so the update is exp(s * ln c) (cheaper than pow, identical result)
    const double kLnFriction = std::log(0.65);
    const double kLnDamping = std::log(0.95);
    const double kLnSlow = std::log(0.8);
    const double kLnRecover = std::log(1.2);
    const double kLnSmooth = std::log(1.0 - 1.0 / 30.0);
    const double kLnGauge = std::log(0.5);

    float g_lastStep = -1.0f;
}

bool installPhysicsFixes() {
    if ((DWORD)GetModuleHandle(NULL) != kImageBase) {
        SDLOG(0, "PhysicsFixes: game image is not at its expected base 0x%08X, nothing patched\n", kImageBase);
        return false;
    }

    // 1. Verify every site first (nothing is written yet).
    bool groupOk[G_COUNT];
    for (int g = 0; g < G_COUNT; ++g) groupOk[g] = true;
    for (const auto& site : g_sites) {
        DWORD currentTarget = 0;
        memcpy(&currentTarget, (const void*)(site.instructionAddress + site.operandOffset), sizeof(DWORD));
        if (currentTarget != site.originalTarget) {
            SDLOG(0, "PhysicsFixes: site 0x%08X reads 0x%08X, expected 0x%08X - group '%s' skipped\n",
                site.instructionAddress, currentTarget, site.originalTarget, kGroupNames[site.group]);
            groupOk[site.group] = false;
        }
    }

    // 2. Patch the groups whose sites all matched.
    int patched = 0, skipped = 0;
    for (const auto& site : g_sites) {
        if (!groupOk[site.group]) { ++skipped; continue; }
        const DWORD targetAddr = site.instructionAddress + site.operandOffset;
        DWORD oldProtect;
        if (VirtualProtect((LPVOID)targetAddr, sizeof(DWORD), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            const DWORD newAddr = (DWORD)site.newTarget;
            memcpy((void*)targetAddr, &newAddr, sizeof(DWORD));
            VirtualProtect((LPVOID)targetAddr, sizeof(DWORD), oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), (LPCVOID)site.instructionAddress, 8);
            ++patched;
        }
        else {
            SDLOG(0, "PhysicsFixes: could not unprotect site 0x%08X (error %lu)\n", site.instructionAddress, GetLastError());
            ++skipped;
        }
    }

    SDLOG(0, "PhysicsFixes: %d sites patched, %d skipped (slide=%d damping=%d graze=%d velocity=%d timers=%d fixedstep=%d gauges=%d)\n",
        patched, skipped, groupOk[G_SLIDE], groupOk[G_DAMPING], groupOk[G_GRAZE], groupOk[G_VELOCITY], groupOk[G_TIMERS],
        groupOk[G_FIXED_STEP], groupOk[G_GAUGE]);
    return skipped == 0;
}

// Called once per frame with the step DSfix just wrote into the engine (seconds).
void updatePhysicsDeltas(float deltaSeconds) {
    if (deltaSeconds < 0.001f) deltaSeconds = 0.001f;
    if (deltaSeconds > 0.125f) deltaSeconds = 0.125f;

    g_PhysicsDeltaTime = deltaSeconds;

    // With the frame limiter the step is very often exactly the same as last frame: nothing to recompute.
    if (deltaSeconds == g_lastStep) return;
    g_lastStep = deltaSeconds;

    const double dt = (double)deltaSeconds;
    const double s = dt * 30.0;
    const double inv = 1.0 / dt;

    // Slide Gravity & Damping
    g_scaledGravity = (float)s;
    g_scaledFriction = (float)std::exp(s * kLnFriction);
    g_scaledDamping = std::exp(s * kLnDamping);

    // Graze Check Scaling
    g_scaledRate = inv;
    g_scaledSlow = std::exp(s * kLnSlow);
    g_scaledRecover = std::exp(s * kLnRecover);

    // Velocity Scaling (Ragdolls & Audio)
    g_invDeltaTime = inv;

    // Timers and Fades
    g_scaledTimerDt = dt;
    g_scaledSmooth = 1.0 - std::exp(s * kLnSmooth);
    g_scaledShineDiv = 2.0 * inv;

    // HUD gauges
    g_gaugeK = 1.0 - std::exp(s * kLnGauge);
    g_gaugeMin = (float)s;

    // Ghost Fixed-Point Timers & Translation Scaling (v1.5.0 logic)
    g_GhostRecStep = -(int32_t)std::lround(s * 65536.0); // rounded (no systematic loss)
    g_GhostTurnScale = (float)s;
    g_GhostStepScale[0] = g_GhostStepScale[1] = g_GhostStepScale[2] = g_GhostStepScale[3] = (float)s;
    g_GhostStepsPerSecond = inv; // the step is clamped to 1 ms above, so at most 1000
}


