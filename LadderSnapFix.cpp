#include "LadderSnapFix.h"
#include "PhysicsFixes.h" // For g_PhysicsDeltaTime
#include "main.h"         // For SDLOG
#include <windows.h>
#include <cstdint>
#include <cstring>

// Ladder & ledge snap.
// The movement controller's frame (fn 0xEC08B0) lifts the physics proxy by the step height (setPosition call at
// 0xEC09BA), moves it, then puts it back down by adding [body+0xB4] (-0.3) to its height (load at 0xEC0AF8).
// When the lift is skipped, as at the bottom of a ladder slide, the -0.3 is still applied every frame: one frame
// at 30 FPS, many at high rates, so the body is pulled through the floor. The snap is capped at the one retail
// frame's worth per episode, spread over frames by step * 30. The ground reach [body+0x208] is limited the same
// way so the body does not stay glued to sloped lips at high frame rates.

namespace {
    constexpr DWORD kLiftCall = 0x00EC09BA;    // call 0x8F2380 (proxy setPosition, lifted)
    constexpr DWORD kSetPosition = 0x008F2380;
    constexpr DWORD kSite = 0x00EC0AF8;        // movss xmm1, [ebx+0xB4]
    constexpr DWORD kReturn = 0x00EC0B00;
    const BYTE kSiteBytes[8] = { 0xF3, 0x0F, 0x10, 0x8B, 0xB4, 0x00, 0x00, 0x00 };
}

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
    // Fibonacci hash: take the HIGH 8 bits of the product. The low bits are the same for all bodies that sit at
    // regular strides in memory, which made many characters fall outside the 32-slot probe.
    const size_t start_idx = (size_t)((((uint32_t)body >> 4) * 2654435761u) >> 24);

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
    // Replaces "movss xmm1, [ebx+0xB4]" with "xmm1 = [ebx+0xB4] * snap_factor(body, proxy y, body y)".
    // All general registers, the flags and every SSE register except the result (xmm1) are preserved: snap_factor is
    // compiled C code that may use xmm0-xmm7 freely, while the game's code around this point does not expect a call.
    __asm {
        pushfd
        pushad
        sub esp, 0x80
        movups [esp + 0x00], xmm0
        movups [esp + 0x20], xmm2
        movups [esp + 0x30], xmm3
        movups [esp + 0x40], xmm4
        movups [esp + 0x50], xmm5
        movups [esp + 0x60], xmm6
        movups [esp + 0x70], xmm7
        movss xmm0, dword ptr[esp + 0xB8]     // proxy y = original [esp+0x14] (0x80 + 0x20 pushad + 4 pushfd)

        sub esp, 0x0C
        movss dword ptr[esp + 4], xmm0        // arg 2: proxy y
        mov eax, dword ptr[ebx + 0x14]
        mov dword ptr[esp + 8], eax           // arg 3: body y (frame-start height)
        mov dword ptr[esp], ebx               // arg 1: body
        call snap_factor
        fstp dword ptr[esp]                   // factor

        movups xmm0, [esp + 0x0C + 0x00]
        movups xmm2, [esp + 0x0C + 0x20]
        movups xmm3, [esp + 0x0C + 0x30]
        movups xmm4, [esp + 0x0C + 0x40]
        movups xmm5, [esp + 0x0C + 0x50]
        movups xmm6, [esp + 0x0C + 0x60]
        movups xmm7, [esp + 0x0C + 0x70]
        movss xmm1, dword ptr[esp]
        add esp, 0x8C

        mulss xmm1, dword ptr[ebx + 0xB4]

        popad
        popfd
        mov eax, 0x00EC0B00
        jmp eax
    }
}

bool installLadderSnapFix() {
    // Safety checks: fixed-base image, the exact original snap instruction, and a lift call that
    // really goes to the proxy's setPosition. Anything else means a different build: nothing is patched.
    if ((DWORD)GetModuleHandle(NULL) != 0x00400000) {
        SDLOG(0, "LadderSnapFix: game image is not at its expected base, skipping ladder fix.\n");
        return false;
    }
    if (memcmp((const void*)kSite, kSiteBytes, sizeof(kSiteBytes)) != 0) {
        SDLOG(0, "LadderSnapFix: snap site 0x%08X does not match this build, skipping ladder fix.\n", kSite);
        return false;
    }
    const BYTE* call = (const BYTE*)kLiftCall;
    int32_t callRel = 0;
    memcpy(&callRel, call + 1, sizeof(callRel));
    if (call[0] != 0xE8 || (DWORD)(kLiftCall + 5 + callRel) != kSetPosition) {
        SDLOG(0, "LadderSnapFix: lift call at 0x%08X is not a call to 0x%08X, skipping ladder fix.\n", kLiftCall, kSetPosition);
        return false;
    }

    DWORD oldProtect;

    // Patch Lift Call first: on its own it only notes which body was lifted and is harmless
    if (VirtualProtect((LPVOID)kLiftCall, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        BYTE* callSite = (BYTE*)kLiftCall;
        *(DWORD*)(callSite + 1) = (DWORD)lift_stub - (kLiftCall + 5);
        VirtualProtect((LPVOID)kLiftCall, 5, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), (LPCVOID)kLiftCall, 5);
    }
    else {
        SDLOG(0, "LadderSnapFix: could not unprotect the lift call (error %lu)\n", GetLastError());
        return false;
    }

    // Patch Snap Site second: it reads the lift note, so it is only switched on once the lift note is being written
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
    else {
        SDLOG(0, "LadderSnapFix: could not unprotect the snap site (error %lu)\n", GetLastError());
        return false;
    }

    SDLOG(0, "LadderSnapFix: Successfully installed ladder and ledge fixes.\n");
    return true;
}
