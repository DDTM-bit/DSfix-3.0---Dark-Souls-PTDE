#include "FootPlacementFix.h"
#include "main.h"        // SDLOG
#include <cmath>
#include <cstring>
#include <cstdint>

// Foot placement on stairs and slopes.
//
// The character's feet are put on the ground by Havok's foot placement solver, one per leg, and the hips are
// lowered so both feet can reach. Three steps of that are fixed fractions PER FRAME, with no frame time in them:
//
//   1. the pelvis offset moves toward the legs' vertical error by 0.2 of it each frame
//      (mulsd xmm0, [0x011E82D0] at 0x00FE1991, the constant being 0.2);
//   2. with no foot on the ground that offset decays by the character's own factor each frame
//      (movss xmm0, [esi+0x2EC] at 0x00FE193C; the character's constructor stores 0.6 there from 0x01161970
//      at 0x00FE254E, and nothing else writes it as a float);
//   3. the solver blends its own weight toward on (1) or off (0) by the gain in its input block each call
//      (weight += (target - weight) * input+0x68 at 0x007B2AFC; the solver 0x007B1A50 is called only from
//      0x00ED4BF4, with the input block as its first stack argument).
//      The 0.2 in the input comes from a template inside the character's foot object (object+0xC0, so the
//      gain is object+0x128). Both writers of the template store the same 0.2: the constructor 0x007B1510
//      and the setup at 0x00ED46DA. Every call, 0x00464250 copies the whole template into a local input
//      block on the caller's stack (esp+0xF0 in 0x00ED4A70), and that local is what the solver reads.
//
// At 120 FPS the pelvis follows about three times as fast as at 30 FPS, so it chases every step of a
// staircase and the whole body bobs while running up or down stairs and slopes.
//
// Each fraction is applied per 1/30 s instead of per frame, with s = step * 30:
//   a gain of g per frame  ->  1 - (1 - g) ^ s     (0.2 -> 1 - 0.8 ^ s)
//   a decay of d per frame ->  d ^ s               (0.6 -> 0.6 ^ s)
// At exactly 30 FPS s is 1 and every value equals the game's own constant, so nothing changes.
//
// HOW THE THREE SITES ARE PATCHED
//
//   1. Pelvis gain: only the 32-bit ADDRESS OPERAND of the load is retargeted to our own double, exactly like
//      PhysicsFixes.cpp. No code byte changes, so no jump can be broken.
//   3. Solver gain: only the 32-bit RELATIVE OPERAND of the call is retargeted to a stub that corrects the
//      gain in the input block and then jumps to the real solver. The instruction keeps its length and its
//      first byte, so again no code byte changes. The stub writes only into the LOCAL copy of the input,
//      which is filled again from the template before every call, so the template itself is never changed
//      and nothing has to be restored afterwards.
//   2. Pelvis decay: this one reads a field of the character, not a constant, so it needs a real hook:
//      8 bytes at 0x00FE193C become a jump to a stub that loads the scaled value instead, and the stub
//      returns to 0x00FE1944. This is the only site where code bytes change, and the 8 bytes are exactly
//      the one movss instruction (F3 0F 10 86 + a 4-byte displacement), so no following instruction is
//      overwritten and no jump target can be swallowed - the fault that caused the archer crash in
//      bulkItemUse cannot happen here.
//
// WHAT IS LEFT, AND WHY IT IS NOT FIXED HERE
//
// With this fix the hips and the camera are calm on stairs, but the feet still tremble a little while going
// down, at 120 FPS more visibly than at 30 FPS. Two causes were considered:
//  * The on/off switch of foot placement (0x00FE17EB..0x00FE180B: placement goes off when the y of
//    [[ctrl+0x1C]+0x130] is outside +-0.0005). A per-frame threshold would flip placement on and off at a
//    high frame rate. MEASURED AND RULED OUT: a diagnostic build recorded every player frame on the Firelink
//    staircase, down and up, at 30 and at 120 FPS. The value was exactly 0 and placement was on in 100% of
//    frames with 0 flips at both rates (the decision read from the call matched the solver input every frame).
//  * The solver's ground gains (input+0x6C..+0x7C) are 1.0: each frame the foot goes all the way onto the
//    ground found under it. That does not depend on the frame rate, but on a stair edge the ground under the
//    ankle jumps between two step heights, and at a high frame rate more frames show the foot crossing it.
//    This is the engine's own behaviour (it is visible at 30 FPS too). Smoothing it would make the feet look
//    different from the original game, so it is deliberately left alone.
//
// DEFENSIVE RULE (nothing is guessed at run time)
//
// Both stubs compare what the game is about to use with the value this build expects (0.6 for the decay,
// 0.2 for the solver gain). Only an exact match is replaced; anything else is left completely alone. So an
// unexpected character or a changed build loses the fix instead of getting a wrong number.
//
// WHEN IT IS ACTIVE
//
// There is no setting: FPS.cpp installs it whenever the frame rate is unlocked (unlockFPS 1) with a limit
// above 30 (FPSlimit 0 counts as 60, as everywhere in DSfix). With the limit at 30 nothing is patched. When the
// game runs at 30 FPS anyway (the 30 FPS hotkey, or a slow frame) s is 1 and every value is the game's own.

namespace {
    constexpr DWORD kImageBase = 0x00400000;

    // 1. Pelvis gain: mulsd xmm0, qword ptr [0x011E82D0]
    constexpr DWORD kGainSite = 0x00FE1991;
    const BYTE kGainBytes[8] = { 0xF2, 0x0F, 0x59, 0x05, 0xD0, 0x82, 0x1E, 0x01 };
    constexpr DWORD kGainOperand = 4;
    constexpr DWORD kGainConstant = 0x011E82D0;  // the double the game loads there (0.2)

    // 2. Pelvis decay: movss xmm0, dword ptr [esi+0x2EC]
    constexpr DWORD kDecaySite = 0x00FE193C;
    const BYTE kDecayBytes[8] = { 0xF3, 0x0F, 0x10, 0x86, 0xEC, 0x02, 0x00, 0x00 };
    constexpr DWORD kDecayReturn = 0x00FE1944;

    // 3. Solver call: call 0x007B1A50
    constexpr DWORD kSolveCall = 0x00ED4BF4;
    constexpr DWORD kSolve = 0x007B1A50;
    constexpr DWORD kSolverGainOffset = 0x68;

    // The values this build expects to find, as raw float bits.
    constexpr DWORD kDecayExpectBits = 0x3F19999A;  // 0.6f
    constexpr DWORD kGainExpectBits = 0x3E4CCCCD;   // 0.2f

    bool g_installed = false;
    float g_lastStep = -1.0f;

    // The game's own pelvis gain, read out of the exe at install time instead of being assumed.
    double g_pelvisGainOriginal = 0.2;
}

// Read by the game itself, or by the stubs below, so these stay at file scope and are not optimised away.
// The pelvis gain is a double because the instruction that reads it is a mulsd.
alignas(8) volatile double g_FootPelvisGain = 0.2;

volatile DWORD g_FootDecayExpect = kDecayExpectBits;  // the per-frame decay this build expects (0.6f)
volatile DWORD g_FootDecayValue = kDecayExpectBits;   // what the stub puts in its place (0.6 ^ s)

volatile DWORD g_FootSolverExpect = kGainExpectBits;  // the solver on/off gain this build expects (0.2f)
volatile DWORD g_FootSolverValue = kGainExpectBits;   // what the stub puts in its place (1 - 0.8 ^ s)

// Replaces "movss xmm0, [esi+0x2EC]" (8 bytes at 0x00FE193C) and returns to 0x00FE1944.
// Only xmm0 is changed: eax and the flags are saved and put back, and the return is done with push/ret so
// that no register has to hold the return address.
__declspec(naked) void footDecayStub() {
    __asm {
        pushfd
        push eax
        mov eax, dword ptr[esi + 0x2EC]
        cmp eax, dword ptr[g_FootDecayExpect]
        jne emit                            // not the expected 0.6: the game's own value is used
        mov eax, dword ptr[g_FootDecayValue]
    emit:
        movd xmm0, eax
        pop eax
        popfd
        push 0x00FE1944
        ret
    }
}

// Stands in front of the foot placement solver (the call at 0x00ED4BF4 is pointed here).
// The solver is a thiscall with the solver object in ecx and three stack arguments; the first one is the
// input block whose +0x68 holds the on/off gain. Every register and the flags are preserved, and the stub
// jumps into the real solver with the stack exactly as the game left it.
__declspec(naked) void footSolverStub() {
    __asm {
        pushfd
        push eax
        push edx
        mov edx, dword ptr[esp + 0x10]      // [esp]=edx [+4]=eax [+8]=flags [+0x0C]=return [+0x10]=input
        test edx, edx
        jz done
        mov eax, dword ptr[edx + 0x68]
        cmp eax, dword ptr[g_FootSolverExpect]
        jne done                            // not the expected 0.2: left as the game wrote it
        mov eax, dword ptr[g_FootSolverValue]
        mov dword ptr[edx + 0x68], eax
    done:
        pop edx
        pop eax
        popfd
        push 0x007B1A50
        ret
    }
}

namespace {
    float bitsToFloat(DWORD bits) {
        float value;
        memcpy(&value, &bits, sizeof(value));
        return value;
    }

    DWORD floatToBits(float value) {
        DWORD bits;
        memcpy(&bits, &value, sizeof(bits));
        return bits;
    }

    // The game's own numbers, used until the first frame is measured: with these the two replacements
    // store exactly what they found, so the game behaves as if nothing were patched.
    void writeVanillaValues() {
        g_FootPelvisGain = g_pelvisGainOriginal;  // 0.2
        g_FootDecayValue = g_FootDecayExpect;     // 0.6
        g_FootSolverValue = g_FootSolverExpect;   // 0.2
    }
}

bool installFootPlacementFix() {
    if ((DWORD)GetModuleHandle(NULL) != kImageBase) {
        SDLOG(0, "FootPlacementFix: game image is not at its expected base 0x%08X, nothing patched\n", kImageBase);
        return false;
    }

    // 1. Check all three sites before writing anything.
    if (memcmp((const void*)kGainSite, kGainBytes, sizeof(kGainBytes)) != 0) {
        SDLOG(0, "FootPlacementFix: pelvis gain site 0x%08X does not match this build, nothing patched\n", kGainSite);
        return false;
    }
    if (memcmp((const void*)kDecaySite, kDecayBytes, sizeof(kDecayBytes)) != 0) {
        SDLOG(0, "FootPlacementFix: pelvis decay site 0x%08X does not match this build, nothing patched\n", kDecaySite);
        return false;
    }
    const BYTE* call = (const BYTE*)kSolveCall;
    int32_t callRel = 0;
    memcpy(&callRel, call + 1, sizeof(callRel));
    if (call[0] != 0xE8 || (DWORD)(kSolveCall + 5 + callRel) != kSolve) {
        SDLOG(0, "FootPlacementFix: the call at 0x%08X does not go to the solver 0x%08X, nothing patched\n",
            kSolveCall, kSolve);
        return false;
    }

    // 2. Read the game's own pelvis gain rather than assuming it, and refuse anything that is not a
    //    sensible per-frame fraction.
    double original = 0.0;
    memcpy(&original, (const void*)kGainConstant, sizeof(original));
    if (!(original > 0.0 && original < 1.0)) {
        SDLOG(0, "FootPlacementFix: the pelvis gain at 0x%08X is %.6f, not a fraction between 0 and 1,"
            " nothing patched\n", kGainConstant, original);
        return false;
    }
    g_pelvisGainOriginal = original;

    // 3. Start from the game's own values, so the first frame is vanilla whatever happens next.
    writeVanillaValues();

    DWORD oldProtect;

    // 4. Pelvis gain: retarget the address operand only.
    const DWORD gainOperandAddress = kGainSite + kGainOperand;
    if (!VirtualProtect((LPVOID)gainOperandAddress, sizeof(DWORD), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        SDLOG(0, "FootPlacementFix: could not unprotect the pelvis gain operand (error %lu)\n", GetLastError());
        return false;
    }
    const DWORD gainTarget = (DWORD)&g_FootPelvisGain;
    memcpy((void*)gainOperandAddress, &gainTarget, sizeof(gainTarget));
    VirtualProtect((LPVOID)gainOperandAddress, sizeof(DWORD), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)kGainSite, sizeof(kGainBytes));

    // 5. Solver call: retarget the relative operand only.
    if (!VirtualProtect((LPVOID)(kSolveCall + 1), sizeof(DWORD), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        SDLOG(0, "FootPlacementFix: could not unprotect the solver call (error %lu)\n", GetLastError());
        return false;
    }
    const DWORD solverRel = (DWORD)footSolverStub - (kSolveCall + 5);
    memcpy((void*)(kSolveCall + 1), &solverRel, sizeof(solverRel));
    VirtualProtect((LPVOID)(kSolveCall + 1), sizeof(DWORD), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)kSolveCall, 5);

    // 6. Pelvis decay: the only site where code bytes are replaced. Done last, because it is the one that
    //    cannot be undone by writing a harmless value.
    if (!VirtualProtect((LPVOID)kDecaySite, sizeof(kDecayBytes), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        SDLOG(0, "FootPlacementFix: could not unprotect the pelvis decay site (error %lu)\n", GetLastError());
        return false;
    }
    BYTE* site = (BYTE*)kDecaySite;
    const DWORD decayRel = (DWORD)footDecayStub - (kDecaySite + 5);
    site[0] = 0xE9; // jmp
    memcpy(site + 1, &decayRel, sizeof(decayRel));
    site[5] = 0x90;
    site[6] = 0x90;
    site[7] = 0x90;
    VirtualProtect((LPVOID)kDecaySite, sizeof(kDecayBytes), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)kDecaySite, sizeof(kDecayBytes));

    g_installed = true;
    g_lastStep = -1.0f;
    SDLOG(0, "FootPlacementFix: installed (gain 0x%08X, decay 0x%08X -> 0x%08X, solver call 0x%08X -> 0x%08X);"
        " the game's values are pelvis gain %.3f, decay %.3f, solver gain %.3f\n",
        kGainSite, kDecaySite, (DWORD)footDecayStub, kSolveCall, (DWORD)footSolverStub,
        g_pelvisGainOriginal, bitsToFloat(g_FootDecayExpect), bitsToFloat(g_FootSolverExpect));
    return true;
}

void updateFootPlacementFix(float deltaSeconds) {
    if (!g_installed) return;

    if (deltaSeconds < 0.001f) deltaSeconds = 0.001f;
    if (deltaSeconds > 0.125f) deltaSeconds = 0.125f;

    // Recompute only when the step really changed (with the frame limiter it usually does not).
    if (deltaSeconds == g_lastStep) return;
    g_lastStep = deltaSeconds;

    double s = (double)deltaSeconds * 30.0;
    if (s < 0.01) s = 0.01;
    if (s > 8.0) s = 8.0;

    // A gain of g per frame becomes 1 - (1 - g) ^ s. The pelvis and the solver both use 0.2 in
    // this build, but each is scaled from its own value so neither depends on the other.
    g_FootPelvisGain = 1.0 - std::pow(1.0 - g_pelvisGainOriginal, s);
    const double solverGain = (double)bitsToFloat(g_FootSolverExpect);
    g_FootSolverValue = floatToBits((float)(1.0 - std::pow(1.0 - solverGain, s)));

    // A decay of d per frame becomes d ^ s.
    const double decay = (double)bitsToFloat(g_FootDecayExpect);
    g_FootDecayValue = floatToBits((float)std::pow(decay, s));
}
