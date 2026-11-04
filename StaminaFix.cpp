#include "StaminaFix.h"
#include "main.h" // For SDLOG
#include <windows.h>
#include <cstring>
#include <cmath>

// Sprint stamina tick.
// The character update (fn 0xE80A00) keeps a timer at [chr+0x168]: while the character spends stamina it adds the
// frame time, and when the sum reaches the interval (0.1 s, the float at 0x12DF014) it takes one point off the
// stamina and then STORES ZERO in the timer (movss [ebx+168h], xmm2 at 0xE80B00, xmm2 being 0). The leftover of the
// last frame is thrown away, so each tick lasts 0.1 s rounded up to whole frames: at awkward frame rates the drain
// runs up to a frame per tick slow. That store becomes a call that subtracts the interval instead, which makes the
// drain exactly 10 points a second at any frame rate. (DSfix never writes a step longer than 0.1 s, so the timer
// cannot build up a growing remainder.)

namespace {
    constexpr DWORD kTickStoreSite = 0x00E80B00;
    constexpr DWORD kTickInterval = 0x012DF014;
    const BYTE kExpect[8] = { 0xF3, 0x0F, 0x11, 0x93, 0x68, 0x01, 0x00, 0x00 }; // movss [ebx+168h], xmm2
}

__declspec(naked) void stamina_tick_stub() {
    __asm {
        pushfd                          // Save flags (the jg after the call needs them)
        fld dword ptr[ebx + 0x168]     // Load the current stamina timer into the FPU stack
        fsub dword ptr ds : [0x012DF014]  // Subtract exactly the tick interval (0.1 s)
        fstp dword ptr[ebx + 0x168]    // Store the remainder back into the timer
        popfd                           // Restore flags
        ret                             // Return to the game execution
    }
}

// ---- Swamp poison build-up (Blighttown): the same speed at any frame rate ----
// The swamp floor (hit material 23) adds SpEffect 4020 every frame. 4020 lasts 0.06 s; when it has ended, the
// SpEffect list update (fn 0xDC3020) starts its next effect 4021, which adds 3 to the poison build-up, and the floor
// adds a new 4020. The end is found on a whole frame and the next effect starts one frame later, so one step takes
// "frames until 0.06 s has passed, plus one": 3 frames = 0.1 s at 30 FPS, but only 0.07 - 0.08 s at 85 FPS and 0.075 s
// at 120 FPS, so the poison bar filled up to a third faster. (The decrease and the poison damage use real time.)
// Above 30 FPS the duration of 4020 in the game's SpEffectParam (read when a 4020 starts) is set every frame so that
// one step takes the whole number of frames nearest to the 30 FPS step (0.1 s). At a 30 FPS step (or slower) the
// game's own 0.06 is used. Only this row changes: it is the only effect in the game whose next effect builds up a
// status (poison, toxic, bleed or curse). No game code is patched.
namespace swampPoison {
    constexpr DWORD kParamMan = 0x013785B4;      // ParamMan; SpEffectParam = +0x224 (count) / +0x228 (param)
    constexpr int kRowId = 4020, kNextId = 4021;
    bool g_on = false, g_gaveUp = false;
    float* g_duration = NULL;                    // the row's duration (SpEffectParam +0x08)
    float g_original = 0.06f;                    // the game's value
    float g_step30 = 0.1f;                       // one step at 30 FPS (whole frames)
    DWORD g_nextLookup = 0;
    int g_lookups = 0;

    // The row of 4020 in the loaded SpEffectParam (the game's PARAM layout: row count +0x0A, rows from +0x30 as
    // { id, data offset, name offset }). NULL until the params are loaded.
    BYTE* findRow() {
        __try {
            const BYTE* man = *(const BYTE* const*)kParamMan;
            if (!man || *(const int*)(man + 0x224) <= 0) return NULL;
            const BYTE* param = *(const BYTE* const*)(man + 0x228);
            if (!param) return NULL;
            BYTE* data = *(BYTE* const*)(param + 0x20);
            if (!data) return NULL;
            const int rows = *(const WORD*)(data + 0x0A);
            for (int i = 0; i < rows; ++i) {
                const DWORD* e = (const DWORD*)(data + 0x30 + i * 12);
                if ((int)e[0] == kRowId) return data + e[1];
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return NULL;
    }

    bool readRow(const BYTE* row, float& duration, int& next) {
        __try { duration = *(const float*)(row + 0x08); next = *(const int*)(row + 0x124); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void writeDuration(float value) {
        __try { if (*g_duration != value) *g_duration = value; }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_duration = NULL; g_gaveUp = true; }
    }

    // once per frame (render thread, FPS.cpp), with the step the engine got this frame
    void update(float step) {
        if (!g_on || g_gaveUp) return;
        if (!g_duration) {
            const DWORD now = GetTickCount();
            if ((int)(now - g_nextLookup) < 0) return;
            g_nextLookup = now + 1000;   // the params are loaded at start-up: one look a second until then
            BYTE* row = findRow();
            if (!row) {
                if (++g_lookups >= 600) { g_gaveUp = true; SDLOG(0, "SwampPoisonFix: SpEffect %d not found, nothing changed\n", kRowId); }
                return;
            }
            float duration = 0.0f;
            int next = -1;
            if (!readRow(row, duration, next) || next != kNextId || !(duration > 0.0f && duration < 1.0f)) {
                g_gaveUp = true;
                SDLOG(0, "SwampPoisonFix: SpEffect %d is not the expected swamp timer (duration %.3f, next %d), nothing changed\n", kRowId, duration, next);
                return;
            }
            g_duration = (float*)(row + 0x08);
            g_original = duration;
            // at 30 FPS: the frames until the duration has passed, plus one
            const int frames30 = (int)ceilf(duration * 30.0f - 0.0001f) + 1;
            g_step30 = frames30 / 30.0f;
            SDLOG(0, "SwampPoisonFix: SpEffect %d found (duration %.3f s, one poison step %.3f s at 30 FPS)\n", kRowId, duration, g_step30);
        }
        if (!(step < 1.0f / 30.0f - 0.0005f)) { writeDuration(g_original); return; }   // 30 FPS or slower: the game's value
        if (step < 0.001f) step = 0.001f;
        // whole frames per step, nearest to the 30 FPS step; the timer ends one frame before the step ends, so the
        // duration lies in the middle of that frame (safe from the frame-time jitter of the frame limiter)
        int frames = (int)(g_step30 / step + 0.5f);
        if (frames < 2) frames = 2;
        writeDuration((frames - 1.5f) * step);
    }
}

void installSwampPoisonFix() {
    swampPoison::g_on = true;
    SDLOG(0, "SwampPoisonFix: on (Blighttown swamp poison builds up as at 30 FPS)\n");
}

void updateSwampPoisonFix(float step) {
    swampPoison::update(step);
}

bool installStaminaFix() {
    BYTE* site = (BYTE*)kTickStoreSite;

    // Safety checks: the exact original instruction, and a sane interval constant
    if (memcmp(site, kExpect, sizeof(kExpect)) != 0) {
        SDLOG(0, "StaminaFix: Memory mismatch at 0x%08X, skipping sprint stamina fix.\n", kTickStoreSite);
        return false;
    }
    const float interval = *(const float*)kTickInterval;
    if (!(interval >= 0.05f && interval <= 0.5f)) {
        SDLOG(0, "StaminaFix: tick interval is %.4f, not what was expected, skipping sprint stamina fix.\n", interval);
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

        SDLOG(0, "StaminaFix: Successfully patched sprint stamina drain (interval %.3f s).\n", interval);
        return true;
    }
    return false;
}
