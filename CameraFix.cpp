#include "CameraFix.h"
#include "main.h"         // For SDLOG
#include "Settings.h"
#include <windows.h>
#include <cstring>

// =====================================================================================================================
// cameraDistance: the follow camera's distance from the hero, as a multiplier of the game's own (1.0 = vanilla).
// ChrFollowCam::Update (0xF02720) picks a LockCamParam row every frame: the area's free camera row (+0x31C, or the
// camera manager's), the lock-on row (+0x318) when byte +0x261 is set, or an event row (+0x310). The camera is in its
// lock-on mode when byte +0x260 or +0x263 is set (the game's own test for the lock-on look-at weights, 0xF0B5E1).
// Row +0 is the target distance (camDistTarget); the camera's distance (+0x188) then moves toward it by the blend weight
// +0x320 (0xF027F6), so every change is soft. The load of the row's distance (0xF027FE) is multiplied by the setting
// while the camera is not in its lock-on mode (lock-on keeps the vanilla distance), or always with cameraDistanceLockOn 1.
// Wall collision works on the result as before. Cutscenes and bow aiming use other cameras.
// The toggleCameraDistance action switches between the setting and vanilla. cameraDistance 1.0: nothing is patched.
// =====================================================================================================================
namespace camdist {
    constexpr DWORD kSite = 0x00F027FE;         // movss xmm1,[eax] (row distance) / movsd xmm3,[0x11E7DD0]
    const BYTE kExpect[12] = { 0xF3, 0x0F, 0x10, 0x08, 0xF2, 0x0F, 0x10, 0x1D, 0xD0, 0x7D, 0x1E, 0x01 };
    DWORD g_cont = 0x00F0280A;
    bool g_installed = false;
    float g_setting = 1.0f;
    volatile float g_factor = 1.0f;             // the setting, or 1.0 when toggled off
    volatile bool g_lockOnToo = false;          // cameraDistanceLockOn: the lock-on camera gets the distance too

    // eax = the LockCamParam row, ebx = the camera. Only xmm1 (the distance) changes; flags and the rest are kept.
    __declspec(naked) void stub() {
        __asm {
            movss xmm1, dword ptr[eax]          // the replaced instruction: the row's distance
            pushfd
            cmp byte ptr[g_lockOnToo], 0        // cameraDistanceLockOn 1: lock-on too
            jne scale
            cmp byte ptr[ebx + 0x260], 0        // lock-on mode (+0x260 or +0x263): vanilla distance
            jne done
            cmp byte ptr[ebx + 0x263], 0
            jne done
        scale:
            mulss xmm1, dword ptr[g_factor]
        done:
            popfd
            movsd xmm3, qword ptr ds:[0x011E7DD0] // the replaced instruction
            jmp dword ptr[g_cont]
        }
    }
}

bool installCameraDistance() {
    using namespace camdist;
    if (g_installed) return true;
    float s = Settings::get().getCameraDistance();
    if (!(s == s)) s = 1.0f;                    // NaN
    if (s < 0.5f) s = 0.5f;
    if (s > 2.0f) s = 2.0f;
    if (s > 0.999f && s < 1.001f) {
        SDLOG(0, "cameraDistance: 1.0 (vanilla), nothing patched\n");
        return false;
    }
    if (memcmp((const void*)kSite, kExpect, sizeof(kExpect)) != 0) {
        SDLOG(0, "cameraDistance: the code at 0x%08X is not what this build should have, nothing patched\n", kSite);
        return false;
    }
    DWORD oldProtect;
    if (!VirtualProtect((LPVOID)kSite, sizeof(kExpect), PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    g_setting = s;
    g_factor = s;
    g_lockOnToo = Settings::get().getCameraDistanceLockOn();
    BYTE* p = (BYTE*)kSite;
    p[0] = 0xE9;
    *(DWORD*)(p + 1) = (DWORD)&stub - (kSite + 5);
    for (DWORD i = 5; i < sizeof(kExpect); ++i) p[i] = 0x90;
    VirtualProtect((LPVOID)kSite, sizeof(kExpect), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)kSite, sizeof(kExpect));
    g_installed = true;
    SDLOG(0, "cameraDistance: on (camera distance x %.2f, %s)\n", s, g_lockOnToo ? "lock-on too" : "lock-on unchanged");
    return true;
}

void toggleCameraDistance() {
    using namespace camdist;
    if (!g_installed) return;
    g_factor = (g_factor == 1.0f) ? g_setting : 1.0f;
}
