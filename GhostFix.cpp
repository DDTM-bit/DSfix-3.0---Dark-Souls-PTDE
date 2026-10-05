#include "GhostFix.h"
#include "PhysicsFixes.h" // For g_PhysicsDeltaTime
#include "main.h"         // For SDLOG
#include <windows.h>
#include <cstring>

extern int g_GhostFrames10;
extern int g_GhostFrames5;
extern float g_PhysicsDeltaTime;

namespace {
    float kThirtyFloat = 30.0f;

    // 1. Playback Reload (Bloodstains)
    __declspec(naked) void play_reload_stub() {
        __asm {
            push eax
            // Set the counter to real frames (e.g., 40 at 120FPS)
            mov eax, dword ptr[g_GhostFrames10]
            mov dword ptr[esi + 0x274], eax

            // Scale Translation Vector in the buffer
            movups xmm0, [esi + 0x250]
            movss xmm1, dword ptr[g_PhysicsDeltaTime]
            mulss xmm1, dword ptr[kThirtyFloat]
            shufps xmm1, xmm1, 0
            mulps xmm0, xmm1
            movups[esi + 0x250], xmm0

            // Scale Rotation Vector in the buffer
            movups xmm0, [esi + 0x260]
            mulps xmm0, xmm1
            movups[esi + 0x260], xmm0

            pop eax
            ret
        }
    }

    // 2. Recording Reload
    __declspec(naked) void rec_reload_stub() {
        __asm {
            push eax
            mov eax, dword ptr[g_GhostFrames10]
            mov dword ptr[ebx + 0x334], eax
            pop eax
            ret
        }
    }

    // 3. Network Reload (Other Players)
    __declspec(naked) void net_reload_stub() {
        __asm {
            push eax
            mov eax, dword ptr[g_GhostFrames5]
            mov dword ptr[ebx + 0x234], eax

            // Scale Network Translation
            movups xmm0, [ebx + 0x280]
            movss xmm1, dword ptr[g_PhysicsDeltaTime]
            mulss xmm1, dword ptr[kThirtyFloat]
            shufps xmm1, xmm1, 0
            mulps xmm0, xmm1
            movups[ebx + 0x280], xmm0

            // Scale Network Rotation
            movups xmm0, [ebx + 0x290]
            mulps xmm0, xmm1
            movups[ebx + 0x290], xmm0

            pop eax
            ret
        }
    }
}

bool installGhostFix() {
    // We target the Reload instructions, NOT the per-frame updates
    DWORD kPlayReload = 0x00E16548;
    DWORD kRecReload = 0x00E1DDDA;
    DWORD kNetReload = 0x00E1DF7F;

    // Safety checks
    if (*(BYTE*)kPlayReload != 0xC7 || *(BYTE*)kRecReload != 0xC7 || *(BYTE*)kNetReload != 0xC7) {
        SDLOG(0, "GhostFix: Memory mismatch, skipping ghost replay fix.\n");
        return false;
    }

    DWORD oldProtect;

    // 1. Patch Playback Reload (10 bytes)
    if (VirtualProtect((LPVOID)kPlayReload, 10, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        BYTE* p = (BYTE*)kPlayReload;
        p[0] = 0xE8;
        *(DWORD*)(p + 1) = (DWORD)play_reload_stub - (kPlayReload + 5);
        memset(p + 5, 0x90, 5); // NOP remainder
        VirtualProtect((LPVOID)kPlayReload, 10, oldProtect, &oldProtect);
    }
    else return false;

    // 2. Patch Recording Reload (10 bytes)
    if (VirtualProtect((LPVOID)kRecReload, 10, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        BYTE* p = (BYTE*)kRecReload;
        p[0] = 0xE8;
        *(DWORD*)(p + 1) = (DWORD)rec_reload_stub - (kRecReload + 5);
        memset(p + 5, 0x90, 5);
        VirtualProtect((LPVOID)kRecReload, 10, oldProtect, &oldProtect);
    }
    else return false;

    // 3. Patch Network Reload (10 bytes)
    if (VirtualProtect((LPVOID)kNetReload, 10, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        BYTE* p = (BYTE*)kNetReload;
        p[0] = 0xE8;
        *(DWORD*)(p + 1) = (DWORD)net_reload_stub - (kNetReload + 5);
        memset(p + 5, 0x90, 5);
        VirtualProtect((LPVOID)kNetReload, 10, oldProtect, &oldProtect);
    }
    else return false;

    SDLOG(0, "GhostFix: Phantoms and bloodstains perfectly synced to native engine framerate.\n");
    return true;
}