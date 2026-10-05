#include "LadderSnapFix.h"
#include "PhysicsFixes.h" // For g_PhysicsDeltaTime
#include "main.h"         // For SDLOG
#include <windows.h>
#include <cstdint>

uint32_t g_lifted[256] = {};

struct Episode {
    const void* body = nullptr;
    float applied = 0.0f;
    DWORD last_ms = 0;
};
Episode g_episodes[16];

struct GroundState {
    const void* body = nullptr;
    float base = 0.0f;
    float written = -1.0f;
    float drop[8] = {};
    unsigned head = 0;
    DWORD last_ms = 0;
};
GroundState g_ground[256];

void ground_reach_step(const void* body, float dt, float proxy_y, float start_y, float snap) {
    DWORD now = GetTickCount();
    GroundState* state = nullptr;
    const size_t start_idx = (((uint32_t)body >> 4) * 2654435761u) & 255;

    for (size_t n = 0; n < 32; ++n) {
        GroundState& s = g_ground[(start_idx + n) & 255];
        if (s.body == body) {
            state = &s;
            break;
        }
        if (!s.body || now - s.last_ms > 60000) {
            s = GroundState();
            s.body = body;
            s.last_ms = now;
            state = &s;
            break;
        }
    }
    if (!state) return;

    const float net = proxy_y + snap - start_y;
    state->last_ms = now;
    state->drop[state->head & 7] = net < 0.0f ? -net : 0.0f;
    state->head++;

    float* reach = (float*)((uint8_t*)body + 0x208);
    float current = *reach;
    if (current != state->written) {
        state->base = current;
    }
    if (!(state->base > 0.0f) || state->base > 4.0f) return;

    int window = dt > 0.0f ? (int)(1.0f / (30.0f * dt) + 0.5f) : 1;
    window = window < 1 ? 1 : (window > 8 ? 8 : window);
    float recent = 0.0f;
    for (int i = 0; i < window - 1; ++i) {
        recent += state->drop[(state->head - 1 - i) & 7];
    }

    float allowed = state->base * 1.2f - recent;
    if (allowed > state->base) allowed = state->base;
    float floor_reach = state->base * 0.05f;
    if (allowed < floor_reach) allowed = floor_reach;

    state->written = allowed;
    *reach = allowed;
}

extern "C" float __cdecl snap_factor(const void* body, float proxy_y, float start_y) {
    const uint32_t key = ((uint32_t)body >> 4) & 0xFF;
    const bool lifted = (g_lifted[key] == (uint32_t)body);
    g_lifted[key] = 0;

    float result = 1.0f;
    float dt = g_PhysicsDeltaTime;
    DWORD now = GetTickCount();

    if (!lifted) {
        Episode* slot = nullptr;
        Episode* stale = &g_episodes[0];
        for (auto& e : g_episodes) {
            if (e.body == body) {
                slot = &e;
                break;
            }
            if (e.last_ms < stale->last_ms) stale = &e;
        }
        if (!slot) {
            slot = stale;
            slot->body = body;
            slot->applied = 0.0f;
        }
        if (now - slot->last_ms > 250) slot->applied = 0.0f;
        slot->last_ms = now;

        float factor = dt * 30.0f;
        if (factor > 1.0f) factor = 1.0f;

        const float kCap = 0.3f;
        const float kStep = 0.3f;
        const float remaining = kCap - slot->applied;

        if (remaining <= 0.0f) {
            result = 0.0f;
        }
        else {
            if (kStep * factor > remaining) factor = remaining / kStep;
            slot->applied += kStep * factor;
            result = factor;
        }
    }
    else {
        for (auto& e : g_episodes) {
            if (e.body == body) e.applied = 0.0f;
        }
    }

    const float b4 = *(const float*)((const uint8_t*)body + 0xB4);
    ground_reach_step(body, dt, proxy_y, start_y, b4 * result);

    return result;
}

__declspec(naked) void lift_stub() {
    __asm {
        push eax
        mov eax, ebx
        shr eax, 4
        and eax, 0xFF
        mov dword ptr[g_lifted + eax * 4], ebx
        pop eax
        mov eax, 0x008F2380
        jmp eax
    }
}

__declspec(naked) void snap_stub() {
    __asm {
        pushfd
        pushad
        sub esp, 0x10
        movups[esp], xmm0
        movss xmm0, dword ptr[esp + 0x48]

        sub esp, 0x0C
        movss dword ptr[esp + 4], xmm0
        mov eax, dword ptr[ebx + 0x14]
        mov dword ptr[esp + 8], eax
        mov dword ptr[esp], ebx
        call snap_factor
        fstp dword ptr[esp]
        movss xmm1, dword ptr[esp]
        add esp, 0x0C

        movups xmm0, [esp]
        add esp, 0x10

        mulss xmm1, dword ptr[ebx + 0xB4]

        popad
        popfd
        mov eax, 0x00EC0B00
        jmp eax
    }
}

bool installLadderSnapFix() {
    DWORD kLiftCall = 0x00EC09BA;
    DWORD kSite = 0x00EC0AF8;

    // Safety check bytes
    if (*(BYTE*)kLiftCall != 0xE8 || *(BYTE*)kSite != 0xF3) {
        SDLOG(0, "LadderSnapFix: Memory mismatch, skipping ladder fix.\n");
        return false;
    }

    DWORD oldProtect;

    // Patch Lift Call
    if (VirtualProtect((LPVOID)kLiftCall, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        BYTE* callSite = (BYTE*)kLiftCall;
        callSite[0] = 0xE8; // call
        *(DWORD*)(callSite + 1) = (DWORD)lift_stub - (kLiftCall + 5);
        VirtualProtect((LPVOID)kLiftCall, 5, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), (LPCVOID)kLiftCall, 5);
    }
    else return false;

    // Patch Snap Site
    if (VirtualProtect((LPVOID)kSite, 8, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        BYTE* site = (BYTE*)kSite;
        site[0] = 0xE9; // jmp
        *(DWORD*)(site + 1) = (DWORD)snap_stub - (kSite + 5);
        site[5] = 0x90; // nop
        site[6] = 0x90; // nop
        site[7] = 0x90; // nop
        VirtualProtect((LPVOID)kSite, 8, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), (LPCVOID)kSite, 8);
    }
    else return false;

    SDLOG(0, "LadderSnapFix: Successfully installed ladder and ledge fixes.\n");
    return true;
}