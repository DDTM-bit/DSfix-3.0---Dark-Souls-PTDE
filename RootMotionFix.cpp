#include "RootMotionFix.h"
#include "main.h"        // SDLOG
#include <cstring>
#include <cstdint>

// Animation root motion at any frame rate.
//
// THE BUG
//
// Characters move by the movement baked into their animations (root motion). Every frame 0x00D98580 asks Havok
// for the blended root motion of all animation layers (call 0x007BF200 at 0x00D98623, its only caller); layers
// that are blending out are left out of it. Havok sums each animation control's motion times its weight and
// divides by the total weight, so the result is the motion of the animations themselves. But when the total
// weight is not above the skeleton's reference-pose threshold [skeleton+0x20] (0.1, set by the constructor at
// 0x007BFA30 from 0x0110B1D8), it divides by the threshold instead (0x007BF5D5): the motion is scaled down by
// total / threshold.
//
// While steering on a steep slope (the Bed of Chaos slide, animations 7063 / 7064) the game starts the steering
// animation again EVERY frame, so the only layer that counts is one frame old. Its weight follows a smooth curve
// over its 0.1667 s blend-in: after one 1/30 s frame it is 0.104, just above 0.1, so at 30 FPS the slide keeps
// its full speed. After one 1/144 s frame it is about 0.012: the motion is cut to about a tenth, and the hero
// crawls, "moving the legs in the air", as soon as he steers. Measured with a hardware write-watch on the
// movement vector: 6.58 units/s at 30 FPS, 2.16 at 144 FPS, in the same animations.
//
// THE FIX
//
// A layer's weight early in its blend grows with the square of its age, so in the root motion function only,
// the threshold is multiplied by (step * 30)^2: the comparison is then the one the game makes at 30 FPS, at any
// frame rate. At 30 FPS and below the factor is exactly 1, so nothing changes there. The pose blending (which
// also uses that threshold elsewhere) is not touched.
//
// HOW IT IS PATCHED (both sites checked byte for byte; the jump scan finds no jump into either, apart from one
// that lands on the first byte of the second site, which the jump keeps)
//
//   0x007BF429  mov ecx, [esp+0x18] ; fld dword ptr [ecx+0x20]      (7 bytes)  -> jmp rootThresholdStubA, 2 nops
//   0x007BF58A  fstp st(0)          ; fld dword ptr [ecx+0x20]      (5 bytes)  -> jmp rootThresholdStubB
//
// Each stub repeats the original instructions, multiplies the loaded threshold by g_RootThresholdScale and
// jumps back. The flags are not live at either site (the game recomputes them before using them again).

namespace {
    constexpr DWORD kImageBase = 0x00400000;

    constexpr DWORD kSiteA = 0x007BF429;
    const BYTE kSiteABytes[7] = { 0x8B, 0x4C, 0x24, 0x18, 0xD9, 0x41, 0x20 };
    constexpr DWORD kSiteB = 0x007BF58A;
    const BYTE kSiteBBytes[5] = { 0xDD, 0xD8, 0xD9, 0x41, 0x20 };
    constexpr DWORD kThresholdConstant = 0x0110B1D8; // the constructor's 0.1, read only to confirm the build

    bool g_installed = false;
    float g_lastStep = -1.0f;
}

// Read by the stubs, so they stay at file scope.
volatile float g_RootThresholdScale = 1.0f;     // (step * 30)^2, at most 1
volatile DWORD g_RootReturnA = 0x007BF430;
volatile DWORD g_RootReturnB = 0x007BF58F;

__declspec(naked) void rootThresholdStubA() {
    __asm {
        mov ecx, dword ptr[esp + 0x18]
        fld dword ptr[ecx + 0x20]
        fmul dword ptr[g_RootThresholdScale]
        jmp dword ptr[g_RootReturnA]
    }
}

__declspec(naked) void rootThresholdStubB() {
    __asm {
        fstp st(0)
        fld dword ptr[ecx + 0x20]
        fmul dword ptr[g_RootThresholdScale]
        jmp dword ptr[g_RootReturnB]
    }
}

namespace {
    bool writeJump(DWORD site, size_t length, const void* target) {
        DWORD oldProtect;
        if (!VirtualProtect((LPVOID)site, length, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
        BYTE* p = (BYTE*)site;
        const DWORD rel = (DWORD)target - (site + 5);
        p[0] = 0xE9;
        memcpy(p + 1, &rel, sizeof(rel));
        for (size_t i = 5; i < length; ++i) p[i] = 0x90;
        VirtualProtect((LPVOID)site, length, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), (LPCVOID)site, length);
        return true;
    }
}

bool installRootMotionFix() {
    if ((DWORD)GetModuleHandle(NULL) != kImageBase) {
        SDLOG(0, "RootMotionFix: game image is not at its expected base 0x%08X, nothing patched\n", kImageBase);
        return false;
    }
    if (memcmp((const void*)kSiteA, kSiteABytes, sizeof(kSiteABytes)) != 0 ||
        memcmp((const void*)kSiteB, kSiteBBytes, sizeof(kSiteBBytes)) != 0) {
        SDLOG(0, "RootMotionFix: the root motion threshold sites do not match this build, nothing patched\n");
        return false;
    }
    float constant = 0.0f;
    memcpy(&constant, (const void*)kThresholdConstant, sizeof(constant));
    if (!(constant > 0.09f && constant < 0.11f)) {
        SDLOG(0, "RootMotionFix: the reference pose threshold is %.4f, not the expected 0.1, nothing patched\n",
            constant);
        return false;
    }

    g_RootThresholdScale = 1.0f; // vanilla until the first frame is measured

    // B first: on its own it only scales by 1 until the update runs, and it is the less used path.
    if (!writeJump(kSiteB, sizeof(kSiteBBytes), (const void*)&rootThresholdStubB)) {
        SDLOG(0, "RootMotionFix: could not patch 0x%08X (error %lu)\n", kSiteB, GetLastError());
        return false;
    }
    if (!writeJump(kSiteA, sizeof(kSiteABytes), (const void*)&rootThresholdStubA)) {
        SDLOG(0, "RootMotionFix: could not patch 0x%08X (error %lu)\n", kSiteA, GetLastError());
        return false;
    }

    g_installed = true;
    g_lastStep = -1.0f;
    SDLOG(0, "RootMotionFix: installed (threshold %.3f scaled by (step*30)^2 at 0x%08X and 0x%08X)\n",
        constant, kSiteA, kSiteB);
    return true;
}

void updateRootMotionFix(float deltaSeconds) {
    if (!g_installed) return;

    if (deltaSeconds != g_lastStep) {
        g_lastStep = deltaSeconds;
        float s = deltaSeconds * 30.0f;
        if (s > 1.0f) s = 1.0f;     // at 30 FPS and below: the game's own threshold
        if (s < 0.01f) s = 0.01f;
        g_RootThresholdScale = s * s;
    }
}
