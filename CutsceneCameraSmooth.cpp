#include "CutsceneCameraSmooth.h"
#include "main.h"   // SDLOG
#include <cstring>
#include <cmath>

// The cutscene camera moves on every frame above 30 FPS.
//
// THE BUG
//
// The real-time cutscenes ("remo") run on the real frame time: the cutscene clock and the characters' animation
// move on every frame. The camera does not. Its path is stored as keys on whole 30 FPS frames (frame number / 30,
// see the key search fn 0x00D09010), and every frame the camera reader fn 0x00D09DA0 takes the two keys around the
// cutscene time, computes how far between them the time is (0..1, kept at [esp+0x4C]) and then simply takes the
// NEAREST key (below 0.5 key A, otherwise key B) for the position and for the rotation. It never blends.
// At 30 FPS every frame lands on a key, so nothing shows. At 60 FPS the camera stands still for 2 frames and then
// jumps, at 120 FPS for 4 frames, at 144 FPS unevenly. The characters move smoothly inside a camera that jumps 30
// times per second, so a close-up (a head or a neck) looks jerky. Seen in a 120 FPS recording of the Frampt
// cutscene: on every other video frame only Frampt and the hero changed, on the others the whole picture moved.
// (The camera zoom is read by fn 0x00D096D0, which does blend smoothly; it is not touched.)
//
// The game also has a smooth reader for the same data (fn 0x00FC2680 -> 0x00D0A1F0, a Hermite curve with stored
// slopes), but it only calls it once at time 0 when a cutscene camera is created (0x00FC1F00), where the slopes do
// not matter. Its slope data is never really used by the game, so it is NOT used here.
//
// THE FIX
//
// Inside the reader the game already uses, at the two places where it picks the nearest key, the position and the
// rotation are blended between the same two keys by the same amount the game computed:
//   position:  A + (B - A) * amount
//   rotation:  the three angles (radians) A + d * amount, where d = B - A taken the short way round (-pi..pi);
//              the game then brings the angles into -pi..pi itself, as it already did for the picked key.
// The 4th value of each key (always 1.0 from fn 0x00D09530) is taken from the key the game picked.
//
// The game's own choice (the nearest key) is kept, exactly as before, when:
//   - the amount is 0 or less, 1 or more, or not a number;
//   - the two keys are not on neighbouring 30 FPS frames (more than 1.5 frames apart): the game cannot have moved
//     the camera smoothly there at 30 FPS either, so nothing is invented;
//   - the two keys are too far apart for one 30 FPS frame: more than kMaxMove metres or an angle step of more than
//     kMaxTurn radians. That is a camera cut, and blending would show one wrong picture in between.
//     A cut found in the position also keeps the game's choice for the rotation of the same camera sample.
// Measured with a trace build at 144 FPS (Frampt and other cutscenes): keys on every 30 FPS frame, largest step
// between two keys 0.16 m and 5.6 degrees, no cut inside a key list (the shot changes are separate parts of the
// cutscene, which the game switches itself). So the cut rule is only a safety net.
//
// Only the cutscene camera uses fn 0x00D09DA0 (its one caller is 0x00FC2084, the camera of a cutscene part); the
// camera only changes what this PC shows, nothing is sent online. No setting: installed from FPS.cpp whenever
// unlockFPS is 1 and the FPS limit is above 30 (at 30 FPS it would change nothing, so nothing is patched there).
//
// HOW IT IS PATCHED (all checked byte for byte at start-up; no outside jump lands inside the patched bytes)
//
// fn 0x00D09DA0 aligns its stack; below "S" means esp in its body (no pushes pending at the two sites):
//   S+0x4C  amount between the keys (float)       S+0x80 / S+0x84  time of key A / key B (seconds)
//   position: key A at S+0x10, key B at S+0x20, the game copies the picked one to S+0x50 (16 bytes)
//   rotation: key A at S+0x10, key B at S+0x30, the game points eax at the picked one and reads it from 0x00D09F24
//
//   0x00D09E9B  lea eax,[esp+10h] / ja / lea eax,[esp+20h] / 4 x movq to [esp+50h]  (31 bytes)
//               -> jmp camPositionStub, 26 x nop     (back to 0x00D09EBA)
//   0x00D09F1A  lea eax,[esp+10h] / ja / lea eax,[esp+30h]                            (10 bytes)
//               -> jmp camRotationStub, 5 x nop      (result written over key A at S+0x10, eax = S+0x10,
//                                                     back to 0x00D09F24)
// xmm3 (amount) and xmm4 (0.5) are still used by the game after the sites, so the stubs save and restore all
// xmm registers and all general registers around the call.

namespace {
    constexpr DWORD kImageBase = 0x00400000;

    constexpr DWORD kPositionSite = 0x00D09E9B;
    constexpr DWORD kPositionSiteLen = 31;
    constexpr DWORD kPositionReturn = 0x00D09EBA;
    constexpr DWORD kRotationSite = 0x00D09F1A;
    constexpr DWORD kRotationSiteLen = 10;
    constexpr DWORD kRotationReturn = 0x00D09F24;

    // Start of fn 0x00D09DA0 up to the store of the amount at [esp+0x4C]: stack layout, key search call,
    // amount = (t - tA) / (tB - tA), the 0.5 used for the nearest key.
    constexpr DWORD kSetupAddr = 0x00D09DA0;
    const BYTE kSetupBytes[145] = {
        0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0xF4, 0x00, 0x00, 0x00, 0x53, 0x8B, 0x5D, 0x0C,
        0x56, 0x57, 0x8D, 0x84, 0x24, 0xC8, 0x00, 0x00, 0x00, 0xE8, 0x52, 0xF2, 0xFF, 0xFF, 0x8B, 0xF0,
        0xB9, 0x0D, 0x00, 0x00, 0x00, 0x8D, 0x7C, 0x24, 0x7C, 0xF3, 0xA5, 0xF3, 0x0F, 0x10, 0x8C, 0x24,
        0x80, 0x00, 0x00, 0x00, 0xF3, 0x0F, 0x10, 0x94, 0x24, 0x84, 0x00, 0x00, 0x00, 0x0F, 0x5A, 0xC1,
        0x0F, 0x5A, 0xDA, 0x66, 0x0F, 0x2E, 0xC3, 0x9F, 0xF6, 0xC4, 0x44, 0x7B, 0x20, 0xF3, 0x0F, 0x10,
        0x44, 0x24, 0x7C, 0x0F, 0x5A, 0xD9, 0x0F, 0x5A, 0xC0, 0x0F, 0x5A, 0xD2, 0x0F, 0x5A, 0xC9, 0xF2,
        0x0F, 0x5C, 0xC3, 0xF2, 0x0F, 0x5C, 0xD1, 0xF2, 0x0F, 0x5E, 0xC2, 0xEB, 0x03, 0x0F, 0x57, 0xC0,
        0x8B, 0x8C, 0x24, 0x88, 0x00, 0x00, 0x00, 0xF3, 0x0F, 0x10, 0x25, 0xA4, 0xFB, 0x15, 0x01, 0x66,
        0x0F, 0x5A, 0xD8, 0x66, 0x0F, 0x6F, 0x05, 0xD0, 0x52, 0x0D, 0x01, 0xF3, 0x0F, 0x11, 0x5C, 0x24,
        0x4C,
    };
    // Position: reads key A and key B, picks the nearest (contains the position site).
    constexpr DWORD kPositionAddr = 0x00D09E46;
    const BYTE kPositionBytes[116] = {
        0x85, 0xC9, 0x7C, 0x70, 0x0F, 0xBE, 0x73, 0x2C, 0x56, 0x53, 0x8D, 0x44, 0x24, 0x38, 0xE8, 0xD7,
        0xF6, 0xFF, 0xFF, 0xF3, 0x0F, 0x7E, 0x00, 0x8B, 0x8C, 0x24, 0x94, 0x00, 0x00, 0x00, 0x66, 0x0F,
        0xD6, 0x44, 0x24, 0x18, 0xF3, 0x0F, 0x7E, 0x40, 0x08, 0x56, 0x53, 0x8D, 0x44, 0x24, 0x40, 0x66,
        0x0F, 0xD6, 0x44, 0x24, 0x28, 0xE8, 0xB0, 0xF6, 0xFF, 0xFF, 0xF3, 0x0F, 0x7E, 0x00, 0x83, 0xC4,
        0x10, 0x0F, 0x2F, 0xE3, 0x66, 0x0F, 0xD6, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x7E, 0x40, 0x08, 0x66,
        0x0F, 0xD6, 0x44, 0x24, 0x28, 0x8D, 0x44, 0x24, 0x10, 0x77, 0x04, 0x8D, 0x44, 0x24, 0x20, 0xF3,
        0x0F, 0x7E, 0x00, 0x66, 0x0F, 0xD6, 0x44, 0x24, 0x50, 0xF3, 0x0F, 0x7E, 0x40, 0x08, 0x66, 0x0F,
        0xD6, 0x44, 0x24, 0x58,
    };
    // Rotation: reads key A and key B, picks the nearest (contains the rotation site).
    constexpr DWORD kRotationAddr = 0x00D09EBA;
    const BYTE kRotationBytes[106] = {
        0x8B, 0x8C, 0x24, 0x98, 0x00, 0x00, 0x00, 0x85, 0xC9, 0x0F, 0x8C, 0x7C, 0x02, 0x00, 0x00, 0x0F,
        0xBE, 0x73, 0x2F, 0x56, 0x53, 0x8D, 0x44, 0x24, 0x38, 0xE8, 0x58, 0xF6, 0xFF, 0xFF, 0xF3, 0x0F,
        0x7E, 0x00, 0x8B, 0x8C, 0x24, 0xA4, 0x00, 0x00, 0x00, 0x66, 0x0F, 0xD6, 0x44, 0x24, 0x18, 0xF3,
        0x0F, 0x7E, 0x40, 0x08, 0x56, 0x53, 0x8D, 0x44, 0x24, 0x30, 0x66, 0x0F, 0xD6, 0x44, 0x24, 0x28,
        0xE8, 0x31, 0xF6, 0xFF, 0xFF, 0xF3, 0x0F, 0x7E, 0x00, 0x83, 0xC4, 0x10, 0x0F, 0x2F, 0xE3, 0x66,
        0x0F, 0xD6, 0x44, 0x24, 0x30, 0xF3, 0x0F, 0x7E, 0x40, 0x08, 0x66, 0x0F, 0xD6, 0x44, 0x24, 0x38,
        0x8D, 0x44, 0x24, 0x10, 0x77, 0x04, 0x8D, 0x44, 0x24, 0x30,
    };

    // Largest camera step between two neighbouring 30 FPS keys that still counts as movement, not a cut.
    constexpr float kMaxMove = 1.0f;    // metres (30 m/s)
    constexpr float kMaxTurn = 0.5f;    // radians, about 29 degrees (about 860 degrees per second)
    constexpr float kKeysPerSecond = 30.0f;
    constexpr float kPi = 3.14159265f;
    constexpr float kTwoPi = 6.28318531f;

    // Offsets in the reader's stack (see above).
    constexpr int kAmount = 0x4C;
    constexpr int kTime = 0x7C;
    constexpr int kTimeA = 0x80;
    constexpr int kTimeB = 0x84;
    constexpr int kKeyA = 0x10;
    constexpr int kPosKeyB = 0x20;
    constexpr int kPosOut = 0x50;
    constexpr int kRotKeyB = 0x30;

    // The position decision of the camera sample being read, so the rotation of the same sample can follow it.
    struct LastPosition {
        const BYTE* frame;
        float time, timeA, timeB;
        bool cut;
        bool valid;
    };
    LastPosition g_lastPosition = {};

    float wrapAngle(float a) { // -pi..pi
        return a - floorf(a / kTwoPi + 0.5f) * kTwoPi;
    }

    float readFloat(const BYTE* frame, int offset) { return *(const float*)(frame + offset); }

    // True when the amount and the key spacing allow a blend at all: the time lies between two keys on
    // neighbouring 30 FPS frames.
    bool canBlend(const BYTE* frame, float amount) {
        if (!(amount > 0.0f) || !(amount < 1.0f)) return false;
        const float apart = (readFloat(frame, kTimeB) - readFloat(frame, kTimeA)) * kKeysPerSecond;
        return apart <= 1.5f;
    }
}

// Game thread, once per cutscene camera sample. frame = S. Writes the position the game reads at S+0x50.
extern "C" void __cdecl cutsceneCameraPosition(BYTE* frame) {
    const float amount = readFloat(frame, kAmount);
    const float* a = (const float*)(frame + kKeyA);
    const float* b = (const float*)(frame + kPosKeyB);
    float out[4];
    memcpy(out, (amount < 0.5f) ? a : b, sizeof(out)); // the game's own choice: key A when 0.5 > amount

    bool cut = false;
    if (canBlend(frame, amount)) {
        const float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
        const float move2 = dx * dx + dy * dy + dz * dz;
        if (!(move2 <= kMaxMove * kMaxMove)) {
            cut = true; // a camera cut (or not a number): keep the game's choice
        }
        else {
            out[0] = a[0] + dx * amount;
            out[1] = a[1] + dy * amount;
            out[2] = a[2] + dz * amount;
        }
    }
    memcpy(frame + kPosOut, out, sizeof(out));

    g_lastPosition.frame = frame;
    g_lastPosition.time = readFloat(frame, kTime);
    g_lastPosition.timeA = readFloat(frame, kTimeA);
    g_lastPosition.timeB = readFloat(frame, kTimeB);
    g_lastPosition.cut = cut;
    g_lastPosition.valid = true;
}

// Game thread, once per cutscene camera sample (after the position). Writes the rotation over key A at S+0x10.
extern "C" void __cdecl cutsceneCameraRotation(BYTE* frame) {
    const float amount = readFloat(frame, kAmount);
    float* a = (float*)(frame + kKeyA);
    const float* b = (const float*)(frame + kRotKeyB);
    float out[4];
    memcpy(out, (amount < 0.5f) ? a : b, sizeof(out)); // the game's own choice

    // Did the position of this same camera sample find a cut?
    bool positionCut = false;
    if (g_lastPosition.valid && g_lastPosition.frame == frame &&
        g_lastPosition.time == readFloat(frame, kTime) &&
        g_lastPosition.timeA == readFloat(frame, kTimeA) &&
        g_lastPosition.timeB == readFloat(frame, kTimeB)) {
        positionCut = g_lastPosition.cut;
    }
    g_lastPosition.valid = false;

    if (!positionCut && canBlend(frame, amount)) {
        float d[3];
        float turn = 0.0f;
        for (int i = 0; i < 3; ++i) {
            d[i] = wrapAngle(b[i] - a[i]);
            const float t = fabsf(d[i]);
            if (t > turn) turn = t;
        }
        if (!(turn == turn)) turn = kPi; // not a number: treat like a cut
        if (turn <= kMaxTurn) {
            for (int i = 0; i < 3; ++i) out[i] = a[i] + d[i] * amount;
        }
        // else: a camera cut, keep the game's choice
    }
    memcpy(a, out, sizeof(out));
}

volatile DWORD g_CutsceneCamPositionReturn = kPositionReturn;
volatile DWORD g_CutsceneCamRotationReturn = kRotationReturn;

__declspec(naked) void camPositionStub() {
    __asm {
        pushad
        sub esp, 0x80
        movups [esp + 0x00], xmm0
        movups [esp + 0x10], xmm1
        movups [esp + 0x20], xmm2
        movups [esp + 0x30], xmm3
        movups [esp + 0x40], xmm4
        movups [esp + 0x50], xmm5
        movups [esp + 0x60], xmm6
        movups [esp + 0x70], xmm7
        lea eax, [esp + 0xA0]           // S: 0x80 saved xmm + 0x20 pushad
        push eax
        call cutsceneCameraPosition
        add esp, 4
        movups xmm0, [esp + 0x00]
        movups xmm1, [esp + 0x10]
        movups xmm2, [esp + 0x20]
        movups xmm3, [esp + 0x30]
        movups xmm4, [esp + 0x40]
        movups xmm5, [esp + 0x50]
        movups xmm6, [esp + 0x60]
        movups xmm7, [esp + 0x70]
        add esp, 0x80
        popad
        jmp dword ptr[g_CutsceneCamPositionReturn]
    }
}

__declspec(naked) void camRotationStub() {
    __asm {
        pushad
        sub esp, 0x80
        movups [esp + 0x00], xmm0
        movups [esp + 0x10], xmm1
        movups [esp + 0x20], xmm2
        movups [esp + 0x30], xmm3
        movups [esp + 0x40], xmm4
        movups [esp + 0x50], xmm5
        movups [esp + 0x60], xmm6
        movups [esp + 0x70], xmm7
        lea eax, [esp + 0xA0]           // S
        push eax
        call cutsceneCameraRotation
        add esp, 4
        movups xmm0, [esp + 0x00]
        movups xmm1, [esp + 0x10]
        movups xmm2, [esp + 0x20]
        movups xmm3, [esp + 0x30]
        movups xmm4, [esp + 0x40]
        movups xmm5, [esp + 0x50]
        movups xmm6, [esp + 0x60]
        movups xmm7, [esp + 0x70]
        add esp, 0x80
        popad
        lea eax, [esp + 0x10]           // the game reads the rotation from [eax]
        jmp dword ptr[g_CutsceneCamRotationReturn]
    }
}

namespace {
    bool writeJump(DWORD site, DWORD length, const void* target) {
        DWORD oldProtect;
        if (!VirtualProtect((LPVOID)site, length, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            SDLOG(0, "CutsceneCameraSmooth: could not patch 0x%08X (error %lu)\n", site, GetLastError());
            return false;
        }
        BYTE* p = (BYTE*)site;
        const DWORD rel = (DWORD)target - (site + 5);
        p[0] = 0xE9;
        memcpy(p + 1, &rel, sizeof(rel));
        for (DWORD i = 5; i < length; ++i) p[i] = 0x90;
        VirtualProtect((LPVOID)site, length, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), (LPCVOID)site, length);
        return true;
    }
}

bool installCutsceneCameraSmooth() {
    if ((DWORD)GetModuleHandle(NULL) != kImageBase) {
        SDLOG(0, "CutsceneCameraSmooth: game image is not at its expected base 0x%08X, nothing patched\n", kImageBase);
        return false;
    }
    if (memcmp((const void*)kSetupAddr, kSetupBytes, sizeof(kSetupBytes)) != 0 ||
        memcmp((const void*)kPositionAddr, kPositionBytes, sizeof(kPositionBytes)) != 0 ||
        memcmp((const void*)kRotationAddr, kRotationBytes, sizeof(kRotationBytes)) != 0) {
        SDLOG(0, "CutsceneCameraSmooth: the cutscene camera reader 0x%08X does not match this build, nothing patched\n",
            kSetupAddr);
        return false;
    }
    memset(&g_lastPosition, 0, sizeof(g_lastPosition));

    // The rotation site first: if the position site then fails, only the rotation is blended, which is still a
    // correct camera (each site works on its own).
    if (!writeJump(kRotationSite, kRotationSiteLen, (const void*)&camRotationStub)) return false;
    if (!writeJump(kPositionSite, kPositionSiteLen, (const void*)&camPositionStub)) {
        SDLOG(0, "CutsceneCameraSmooth: only the rotation is smoothed\n");
        return false;
    }

    SDLOG(0, "CutsceneCameraSmooth: installed (cutscene camera blended between its 1/30 s keys at 0x%08X and 0x%08X)\n",
        kPositionSite, kRotationSite);
    return true;
}
