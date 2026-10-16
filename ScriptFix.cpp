#include "ScriptFix.h"
#include "Settings.h" // SDLOG reads the log level when RELEASE_VER is not defined
#include "main.h"
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <initializer_list>

// Game script memory.
//
// The game's Lua 5.0 never starts a garbage collection on its own. The game collects each script state in full
// (fn 0x603E60) when a countdown kept beside it runs out:
//   AI scripts (AiLuaMan, [[0x137DC74] + 0xF90]): [+0xC] -= step; at 0 it is reset to 0.5 s
//   the other state ([[0x137D790] + 4]):           [+0x58] -= step; at 0 it is reset to 1 s
// The step is the engine's 1/30 that DSfix rewrites with the frame time. At 30 FPS that was a clean-up every 15 / 30
// frames; the scripts make garbage every frame, so at 120 FPS four times as much piled up between clean-ups, and in a
// crowded area the script memory could fill up and the game closed itself.
// The countdown is compiled into 8 places. Each converts the timer and the step to double and runs
//   subsd xmm0, xmm1 ; xorps xmm1, xmm1          (timer - step, then xmm1 = 0 for the comparison)
// Those 7 bytes become a call to a stub that subtracts max(step, 1/30) instead: a clean-up at least every 15 / 30
// frames as at 30 FPS, and still every 0.5 / 1 s below 30 FPS (there the step is already longer than 1/30).
//
// SCRIPT: log line (only in builds where SDLOG writes, and only with logLevel 1 or higher): a script error that nothing
// catches makes the game call exit(1) with no message. luaD_throw (fn 0x698070) runs the panic function [G + 0x28] and
// then exit(1) when there is no error handler; the first 6 bytes of that path go to a stub that writes one log line
// first. It only runs at the moment the game closes itself, so it costs nothing during play.

namespace {
    const DWORD kCountdownSites[] = {
        0x00EFC8DA,  // AI, in-game step
        0x00EFC888,  // other, in-game step
        0x00EFE86E,  // AI, second step
        0x00EFE821,  // other, second step
        0x00CF70BC,  // AI, standalone (with a "now" flag)
        0x00D5D9DC,  // other, standalone (with a "now" flag)
        0x00CA9156,  // AI, standalone copies with no caller found
        0x00CA9206,
    };
    const BYTE kCountdownCode[7] = { 0xF2, 0x0F, 0x5C, 0xC1, 0x0F, 0x57, 0xC9 }; // subsd xmm0,xmm1 ; xorps xmm1,xmm1

    __declspec(align(8)) const double g_minStep = 1.0 / 30.0;

    bool writeCode(DWORD address, const BYTE* bytes, size_t size) {
        DWORD oldProtect;
        if (!VirtualProtect((LPVOID)address, size, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
        memcpy((void*)address, bytes, size);
        FlushInstructionCache(GetCurrentProcess(), (LPCVOID)address, size);
        VirtualProtect((LPVOID)address, size, oldProtect, &oldProtect);
        return true;
    }

    // opcode 0xE8 (call) or 0xE9 (jmp) to target, the rest of `size` filled with nops
    bool writeBranch(BYTE opcode, DWORD site, size_t size, const void* target) {
        BYTE bytes[16];
        if (size < 5 || size > sizeof(bytes)) return false;
        bytes[0] = opcode;
        const int32_t rel = (int32_t)((DWORD)target - (site + 5));
        memcpy(bytes + 1, &rel, 4);
        for (size_t i = 5; i < size; ++i) bytes[i] = 0x90;
        return writeCode(site, bytes, size);
    }

#ifndef RELEASE_VER
    const DWORD kPanicPath = 0x00698090;
    const DWORD kPanicResume = 0x00698096;
    const BYTE kPanicCode[6] = { 0x8B, 0x48, 0x10, 0x8B, 0x51, 0x28 }; // mov ecx,[eax+10h] ; mov edx,[ecx+28h]

    // Pointer chain to a state's lua_State: root global, manager, DLLua, L ([DLLua + 0x18]).
    const uint8_t* scriptState(DWORD root, DWORD managerOffset, DWORD luaOffset) {
        __try {
            const uint8_t* base = *(const uint8_t* const*)root;
            if (!base) return nullptr;
            const uint8_t* manager = *(const uint8_t* const*)(base + managerOffset);
            if (!manager) return nullptr;
            const uint8_t* lua = *(const uint8_t* const*)(manager + luaOffset);
            return lua ? *(const uint8_t* const*)(lua + 0x18) : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    // global_State = [L + 0x10], bytes in use = [G + 0x24]
    bool scriptBytes(const uint8_t* state, uint32_t* bytes) {
        if (!state) return false;
        __try {
            const uint8_t* global = *(const uint8_t* const*)(state + 0x10);
            if (!global) return false;
            *bytes = *(const uint32_t*)(global + 0x24);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // A runtime error leaves its message on top of the stack: TObject {int tt; Value at +8} (16 bytes) at
    // L->top ([L + 8]) - 16; a string (tt 4) is a TString with the length at +0xC and the text at +0x10.
    void errorMessage(const uint8_t* state, char* message, size_t size) {
        message[0] = 0;
        __try {
            const uint8_t* top = *(const uint8_t* const*)(state + 8);
            if (*(const int*)(top - 0x10) == 4) {
                const uint8_t* text = *(const uint8_t* const*)(top - 0x08);
                const uint32_t length = *(const uint32_t*)(text + 0x0C);
                const uint32_t n = length < size - 1 ? length : (uint32_t)(size - 1);
                memcpy(message, text + 0x10, n);
                message[n] = 0;
                for (uint32_t i = 0; i < n; ++i) {
                    if ((unsigned char)message[i] < 0x20) message[i] = ' ';
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            message[0] = 0;
        }
    }

    void __cdecl logPanic(const uint8_t* state, int code) {
        static const char* const kCodes[] = { "no error", "runtime error", "file error", "syntax error", "out of memory",
                                              "error in the error handler" };
        const char* kind = (code >= 0 && code <= 5) ? kCodes[code] : "unknown error";
        const char* which = state == scriptState(0x0137DC74, 0xF90, 8) ? "the AI scripts"
            : (state == scriptState(0x0137D790, 4, 4) ? "the other script state" : "a script state");
        uint32_t bytes = 0;
        const double inUse = scriptBytes(state, &bytes) ? bytes / 1048576.0 : -1.0;
        char message[160];
        if (code == 1) errorMessage(state, message, sizeof(message));
        else message[0] = 0;
        SDLOG(0, "SCRIPT: %s hit an error that nothing catches (%s, Lua error %d); the game closes itself now "
            "(script memory in use: %.2f MB)%s%s\n", which, kind, code, inUse, message[0] ? ": " : "", message);
    }
#endif
}

bool installScriptFix() {
    // 1. Verify all 8 countdown sites before anything is written (all or nothing)
    bool countdownsMatch = true;
    for (DWORD site : kCountdownSites) {
        if (memcmp((const void*)site, kCountdownCode, sizeof(kCountdownCode)) != 0) {
            SDLOG(0, "ScriptFix: collection countdown at 0x%08X does not match this build\n", site);
            countdownsMatch = false;
        }
    }

    BYTE* cave = (BYTE*)VirtualAlloc(nullptr, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!cave) return false;
    BYTE* p = cave;
    auto emit = [&](std::initializer_list<BYTE> bytes) { for (BYTE b : bytes) *p++ = b; };
    auto emit32 = [&](DWORD v) { memcpy(p, &v, 4); p += 4; };

    // Countdown stub: xmm0 = timer, xmm1 = step (both double). The comparison after the call sets its own flags.
    BYTE* countdown = p;
    emit({ 0xF2, 0x0F, 0x5F, 0x0D });   // maxsd xmm1, [g_minStep]
    emit32((DWORD)&g_minStep);
    emit({ 0xF2, 0x0F, 0x5C, 0xC1 });   // subsd xmm0, xmm1
    emit({ 0x0F, 0x57, 0xC9 });         // xorps xmm1, xmm1
    emit({ 0xC3 });                     // ret

#ifndef RELEASE_VER
    // Uncaught script error: esp is as on entry to luaD_throw ([esp+4] = L, [esp+8] = code) and eax = L.
    BYTE* panic = p;
    emit({ 0x50 });                     // push eax
    emit({ 0xFF, 0x74, 0x24, 0x0C });   // push dword ptr [esp+0Ch]   (code)
    emit({ 0x50 });                     // push eax                   (L)
    emit({ 0xB9 });                     // mov ecx, logPanic
    emit32((DWORD)&logPanic);
    emit({ 0xFF, 0xD1 });               // call ecx
    emit({ 0x83, 0xC4, 0x08 });         // add esp, 8
    emit({ 0x58 });                     // pop eax
    for (BYTE b : kPanicCode) *p++ = b; // mov ecx,[eax+10h] ; mov edx,[ecx+28h]
    emit({ 0xE9 });                     // jmp back (push eax ; call edx = the panic function, then exit(1))
    emit32(kPanicResume - ((DWORD)p + 4));
#endif
    FlushInstructionCache(GetCurrentProcess(), cave, 256);

    bool ok = countdownsMatch;
    if (countdownsMatch) {
        for (DWORD site : kCountdownSites) {
            if (!writeBranch(0xE8, site, sizeof(kCountdownCode), countdown)) {
                SDLOG(0, "ScriptFix: could not patch the collection countdown at 0x%08X\n", site);
                ok = false;
            }
        }
    }
    SDLOG(0, "ScriptFix: script garbage collection %s\n",
        ok ? "at least every 15 / 30 frames, as at 30 FPS (8 sites)" : "NOT patched (this build differs)");

#ifndef RELEASE_VER
    // The SCRIPT: line can only be written when SDLOG writes level-0 lines (logLevel 1 or higher)
    if (Settings::get().getLogLevel() > 0) {
        if (memcmp((const void*)kPanicPath, kPanicCode, sizeof(kPanicCode)) == 0 &&
            writeBranch(0xE9, kPanicPath, sizeof(kPanicCode), panic)) {
            SDLOG(0, "ScriptFix: script errors that close the game are logged (SCRIPT: lines)\n");
        }
        else {
            SDLOG(0, "ScriptFix: script error path at 0x%08X does not match this build, SCRIPT: lines off\n", kPanicPath);
        }
    }
#endif
    return ok;
}
