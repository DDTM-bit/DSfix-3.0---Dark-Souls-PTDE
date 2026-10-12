#include "GhostFix.h"
#include "main.h" // For SDLOG
#include <windows.h>
#include <cstring>

extern float g_GhostUnitsPerSecond;
extern int32_t g_GhostRecStep;
extern float g_GhostTurnScale;
extern float g_GhostStepScale[4];

namespace {
    // 1. Playback Counter Stub (Scaled by 65536)
    __declspec(naked) void play_dec_stub() {
        __asm {
            mulss xmm0, dword ptr[g_GhostUnitsPerSecond]
            cvtss2si eax, xmm0
            sub dword ptr[esi + 0x274], eax
            ret
        }
    }

    // 2. Recording Counter Stub
    __declspec(naked) void rec_dec_stub() {
        __asm {
            mov edi, dword ptr[g_GhostRecStep]
            add dword ptr[ebx + 0x234], edi
            ret
        }
    }

    // 3. Remote Turn Stub
    __declspec(naked) void remote_turn_stub() {
        __asm {
            movups xmm0, [edi + 0x290]
            movss xmm1, dword ptr[g_GhostTurnScale]
            shufps xmm1, xmm1, 0
            mulps xmm0, xmm1
            movups[edi + 0x20], xmm0
            mov ecx, dword ptr[ebp + 0x0C]
            ret
        }
    }

    // 4. Replay Turn Stub
    __declspec(naked) void replay_turn_stub() {
        __asm {
            movups xmm0, [esi + 0x260]
            movss xmm1, dword ptr[g_GhostTurnScale]
            shufps xmm1, xmm1, 0
            mulps xmm0, xmm1
            movups[esi + 0x20], xmm0
            ret
        }
    }

    // 5. Replay Move Stub (The missing link!)
    __declspec(naked) void replay_move_stub() {
        __asm {
            lea edx, [ecx + 0x250]
            cmp eax, edx
            movq xmm0, qword ptr[eax]
            jne skip1
            mulps xmm0, xmmword ptr[g_GhostStepScale]
            skip1:
            movq qword ptr[ecx + 0x70], xmm0
                movq xmm0, qword ptr[eax + 8]
                jne skip2
                mulps xmm0, xmmword ptr[g_GhostStepScale]
                skip2 :
                movq qword ptr[ecx + 0x78], xmm0
                ret
        }
    }

    // Helper to safely rewrite MOV instructions to ADD instructions
    bool convert_mov_to_add(DWORD site, int frames) {
        DWORD oldProtect;
        if (VirtualProtect((LPVOID)site, 10, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            BYTE* p = (BYTE*)site;
            p[0] = 0x81; // ADD opcode
            int32_t val = frames * 65536; // Scale up for fixed-point
            memcpy(p + 6, &val, 4);
            VirtualProtect((LPVOID)site, 10, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), (LPCVOID)site, 10);
            return true;
        }
        return false;
    }
}

bool installGhostFix() {
    const DWORD kPlayDec = 0x00E16533;     // dec dword ptr [esi+274h]                (6 bytes)
    const DWORD kPlayReload = 0x00E16548;  // mov dword ptr [esi+274h], 0Ah           (10 bytes)
    const DWORD kRecDec = 0x00E1DC71;      // or edi,-1 ; add [ebx+234h],edi           (9 bytes)
    const DWORD kRecReload = 0x00E1DDDA;   // mov dword ptr [ebx+334h], 0Ah           (10 bytes)
    const DWORD kNetReload = 0x00E1DF7F;   // mov dword ptr [ebx+234h], 5             (10 bytes)
    const DWORD kRemoteTurn = 0x00E203D4;  // movq xmm0,[edi+290h] .. movq [edi+28h]   (29 bytes)
    const DWORD kReplayTurn = 0x00E16B2A;  // movq xmm0,[esi+260h] .. movq [esi+28h]   (26 bytes)
    const DWORD kReplayMove = 0x00E15EEC;  // movq xmm0,[eax] .. movq [ecx+78h],xmm0   (19 bytes)

    // Safety checks (as in zofrea): every site is compared byte for byte BEFORE anything is written,
    // so a different build is never left half patched.
    static const BYTE kPlayDecExpect[6] = { 0xFF, 0x8E, 0x74, 0x02, 0x00, 0x00 };
    static const BYTE kPlayReloadExpect[10] = { 0xC7, 0x86, 0x74, 0x02, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00 };
    static const BYTE kRecDecExpect[9] = { 0x83, 0xCF, 0xFF, 0x01, 0xBB, 0x34, 0x02, 0x00, 0x00 };
    static const BYTE kRecReloadExpect[10] = { 0xC7, 0x83, 0x34, 0x03, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00 };
    static const BYTE kNetReloadExpect[10] = { 0xC7, 0x83, 0x34, 0x02, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00 };
    static const BYTE kRemoteTurnExpect[29] = { 0xF3, 0x0F, 0x7E, 0x87, 0x90, 0x02, 0x00, 0x00,
                                                0x66, 0x0F, 0xD6, 0x47, 0x20,
                                                0x8B, 0x4D, 0x0C,
                                                0xF3, 0x0F, 0x7E, 0x87, 0x98, 0x02, 0x00, 0x00,
                                                0x66, 0x0F, 0xD6, 0x47, 0x28 };
    static const BYTE kReplayTurnExpect[26] = { 0xF3, 0x0F, 0x7E, 0x86, 0x60, 0x02, 0x00, 0x00,
                                                0x66, 0x0F, 0xD6, 0x46, 0x20,
                                                0xF3, 0x0F, 0x7E, 0x86, 0x68, 0x02, 0x00, 0x00,
                                                0x66, 0x0F, 0xD6, 0x46, 0x28 };
    static const BYTE kReplayMoveExpect[19] = { 0xF3, 0x0F, 0x7E, 0x00,
                                                0x66, 0x0F, 0xD6, 0x41, 0x70,
                                                0xF3, 0x0F, 0x7E, 0x40, 0x08,
                                                0x66, 0x0F, 0xD6, 0x41, 0x78 };
    struct Check { DWORD va; const BYTE* bytes; size_t size; };
    const Check checks[] = {
        { kPlayDec, kPlayDecExpect, sizeof(kPlayDecExpect) },
        { kPlayReload, kPlayReloadExpect, sizeof(kPlayReloadExpect) },
        { kRecDec, kRecDecExpect, sizeof(kRecDecExpect) },
        { kRecReload, kRecReloadExpect, sizeof(kRecReloadExpect) },
        { kNetReload, kNetReloadExpect, sizeof(kNetReloadExpect) },
        { kRemoteTurn, kRemoteTurnExpect, sizeof(kRemoteTurnExpect) },
        { kReplayTurn, kReplayTurnExpect, sizeof(kReplayTurnExpect) },
        { kReplayMove, kReplayMoveExpect, sizeof(kReplayMoveExpect) } };
    for (const Check& c : checks) {
        if (memcmp((const void*)c.va, c.bytes, c.size) != 0) {
            SDLOG(0, "GhostFix: site 0x%08X does not match this build, skipping ghost replay fix.\n", c.va);
            return false;
        }
    }

    DWORD oldProtect;

    // Patch Playback Counter (6 bytes)
    if (VirtualProtect((LPVOID)kPlayDec, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        BYTE* p = (BYTE*)kPlayDec;
        p[0] = 0xE8; *(DWORD*)(p + 1) = (DWORD)play_dec_stub - (kPlayDec + 5);
        p[5] = 0x90;
        VirtualProtect((LPVOID)kPlayDec, 6, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), (LPCVOID)kPlayDec, 6);
    }
    else return false;

    // Patch Recording Counter (9 bytes)
    if (VirtualProtect((LPVOID)kRecDec, 9, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        BYTE* p = (BYTE*)kRecDec;
        p[0] = 0xE8; *(DWORD*)(p + 1) = (DWORD)rec_dec_stub - (kRecDec + 5);
        memset(p + 5, 0x90, 4);
        VirtualProtect((LPVOID)kRecDec, 9, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), (LPCVOID)kRecDec, 9);
    }
    else return false;

    // Patch Remote Turn (29 bytes)
    if (VirtualProtect((LPVOID)kRemoteTurn, 29, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        BYTE* p = (BYTE*)kRemoteTurn;
        p[0] = 0xE8; *(DWORD*)(p + 1) = (DWORD)remote_turn_stub - (kRemoteTurn + 5);
        memset(p + 5, 0x90, 24);
        VirtualProtect((LPVOID)kRemoteTurn, 29, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), (LPCVOID)kRemoteTurn, 29);
    }
    else return false;

    // Patch Replay Turn (26 bytes)
    if (VirtualProtect((LPVOID)kReplayTurn, 26, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        BYTE* p = (BYTE*)kReplayTurn;
        p[0] = 0xE8; *(DWORD*)(p + 1) = (DWORD)replay_turn_stub - (kReplayTurn + 5);
        memset(p + 5, 0x90, 21);
        VirtualProtect((LPVOID)kReplayTurn, 26, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), (LPCVOID)kReplayTurn, 26);
    }
    else return false;

    // Patch Replay Move (19 bytes)
    if (VirtualProtect((LPVOID)kReplayMove, 19, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        BYTE* p = (BYTE*)kReplayMove;
        p[0] = 0xE8; *(DWORD*)(p + 1) = (DWORD)replay_move_stub - (kReplayMove + 5);
        memset(p + 5, 0x90, 14);
        VirtualProtect((LPVOID)kReplayMove, 19, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), (LPCVOID)kReplayMove, 19);
    }
    else return false;

    // Convert fixed 10/5-frame reloads into fixed-point 65536 ADD operations
    convert_mov_to_add(kPlayReload, 10);
    convert_mov_to_add(kRecReload, 10);
    convert_mov_to_add(kNetReload, 5);

    SDLOG(0, "GhostFix: Phantoms and bloodstains perfectly patched (v1.5.0 logic).\n");
    return true;
}
