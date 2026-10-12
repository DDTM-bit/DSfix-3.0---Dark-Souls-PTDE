#include "CameraFix.h"
#include "PhysicsFixes.h" // For g_PhysicsDeltaTime
#include "main.h"         // For SDLOG
#include <windows.h>
#include <cmath>
#include <cstring>
#include <cstdint>

// Follow camera smoothing (original research: zofrea, camera.cpp).
// ChrFollowCam::Update (0xF02720, stdcall(this, dt, chr, extra)) blends many values toward a target by a fixed
// per-frame weight. Two weights live in the camera object (+0x238, +0x1BC) and are rescaled before each update;
// 16 more are read at the point of use and are redirected through a stub that returns 1 - (1 - w)^n (n = dt * 30):
//   [+0x1C0]/[+0x1C4] look-at while locked on, [+0x1A4]/[+0x1B0] the same unlocked, [+0x190] pivot,
//   [+0x1A0] yaw settle (x87 fmul), [+0x234] auto turn, [+0x288] stick smoothing, [+0x320] parameter changes,
//   [+0x130] look-at boost after turning the camera,
// and the double 0.1 at 0x11E7CD0 eases the camera distance.

namespace {
    constexpr uint32_t kFollowUpdate = 0x00F02720;
    const uint32_t kCallSites[] = { 0x00F148DD, 0x00F148FE };

    using FollowFn = void(__stdcall*)(void*, float, void*, void*);
    FollowFn g_orig = (FollowFn)kFollowUpdate;

    const uint32_t kFields[] = { 0x238, 0x1BC };
    constexpr int kFieldCount = 2;

    struct Field {
        float original = 0.0f;
        float written = -1.0f;
    };

    struct CameraObj {
        void* object = nullptr;
        Field field[kFieldCount];
    };
    CameraObj g_cameras[4];

    CameraObj* camera_slot(void* object) {
        CameraObj* free_slot = nullptr;
        for (auto& c : g_cameras) {
            if (c.object == object) return &c;
            if (!c.object && !free_slot) free_slot = &c;
        }
        if (!free_slot) free_slot = &g_cameras[0];
        *free_slot = CameraObj(); // a reused slot must not carry the previous camera's values
        free_slot->object = object;
        return free_slot;
    }

    // Weights up to 0.5 are blend amounts (new = old + (target - old) * k); larger values are per-frame
    // multipliers of the remaining error (error *= m), which scale by a plain power.
    float scaled(float original, double n) {
        if (!(original > 0.0f) || original >= 1.0f) return original;
        if (original <= 0.5f) return (float)(1.0 - std::pow(1.0 - (double)original, n));
        return (float)std::pow((double)original, n);
    }

    bool read_field(void* object, uint32_t offset, float* out) {
        __try {
            *out = *(volatile float*)((uint8_t*)object + offset);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool write_field(void* object, uint32_t offset, float value) {
        __try {
            *(volatile float*)((uint8_t*)object + offset) = value;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void scale_camera(void* object, double n) {
        if (!object) return;
        CameraObj* cam = camera_slot(object);
        for (int i = 0; i < kFieldCount; ++i) {
            float value = 0.0f;
            if (!read_field(object, kFields[i], &value)) return;
            Field& f = cam->field[i];
            if (value != f.written) {
                f.original = value; // the game (re)wrote it: that is the 30 FPS value
            }
            const float target = scaled(f.original, n);
            f.written = target;
            write_field(object, kFields[i], target);
        }
    }

    float g_gain_n = 1.0f;     // dt * 30 of the update in flight
    float g_fmul_tmp = 0.0f;   // operand for the redirected fmul
    alignas(8) double g_dist_k = 0.1;
    double g_last_n = -1.0;

    // stdcall(raw weight bits) -> eax = scaled weight bits: 1 - (1 - w)^n on the x87 unit.
    // Preserves every other register and the flags.
    __declspec(naked) void gain_thunk() {
        __asm {
            mov eax, [esp + 4]
            pushfd
            test eax, eax
            jle done                 // w <= 0: unchanged
            cmp eax, 0x3F800000 // 1.0f
            jge done                 // w >= 1: unchanged
            sub esp, 4
            fld dword ptr[g_gain_n]
            fld1
            fsub dword ptr[esp + 12]
            fyl2x
            fld st(0)
            frndint
            fsub st(1), st
            fxch st(1)
            f2xm1
            fld1
            faddp st(1), st
            fscale
            fstp st(1)
            fld1
            fsubrp st(1), st
            fstp dword ptr[esp]
            mov eax, [esp]
            add esp, 4
            done:
            popfd
                ret 4
        }
    }

    void __stdcall follow_stub(void* camera, float dt, void* chr, void* extra) {
        // DSfix 3.0: use the dt the game hands to this update (as zofrea does), not a global frame time
        double n = (double)dt * 30.0;
        if (!(n > 0.02)) n = 0.02;
        if (n > 10.0) n = 10.0;

        g_gain_n = (float)n;
        if (n != g_last_n) { // the step repeats most frames with the frame limiter: skip the pow
            g_last_n = n;
            g_dist_k = 1.0 - std::pow(1.0 - 0.1, n);
        }

        scale_camera(camera, n);
        g_orig(camera, dt, chr, extra);
    }

    struct GainSite {
        uint32_t va;
        uint32_t offset;
        int xmm; // destination of `movss xmmN, [ebx+offset]`, or -1 for `fmul dword ptr [ebx+offset]`
    };

    const GainSite kGainSites[] = {
        {0x00F0B5F3, 0x1A4, 0}, {0x00F0B5FD, 0x1C0, 0}, {0x00F0B61A, 0x1B0, 1}, {0x00F0B624, 0x1C4, 1},
        {0x00F04FA5, 0x190, 0}, {0x00F079D3, 0x1A0, -1}, {0x00F075D2, 0x234, 2}, {0x00F076CA, 0x234, 2},
        {0x00F06EEB, 0x288, 2}, {0x00F06F31, 0x288, 2}, {0x00F02814, 0x320, 2}, {0x00F02859, 0x320, 2},
        {0x00F02892, 0x320, 2}, {0x00F028D1, 0x320, 2}, {0x00F0297B, 0x320, 2},
        {0x00F0C64C, 0x130, 0} // look-at boost
    };
    constexpr uint32_t kDistSite = 0x00F029B4;  // mulsd xmm1, qword ptr [0x11E7CD0]  (0.1)
    constexpr uint32_t kDistConst = 0x011E7CD0;

    void expected_load(const GainSite& g, uint8_t* bytes, size_t* size) {
        if (g.xmm < 0) {
            bytes[0] = 0xD8; bytes[1] = 0x8B; // fmul dword ptr [ebx+disp32]
            memcpy(bytes + 2, &g.offset, 4);
            *size = 6;
        }
        else {
            bytes[0] = 0xF3; bytes[1] = 0x0F; bytes[2] = 0x10; // movss xmmN, dword ptr [ebx+disp32]
            bytes[3] = (uint8_t)(0x83 | (g.xmm << 3));
            memcpy(bytes + 4, &g.offset, 4);
            *size = 8;
        }
    }

    bool install_gain_sites() {
        // Verify everything first (as in zofrea); nothing is written if one site differs
        for (const GainSite& g : kGainSites) {
            uint8_t want[8];
            size_t size = 0;
            expected_load(g, want, &size);
            if (memcmp((const void*)g.va, want, size) != 0) {
                SDLOG(0, "CameraFix: gain load at 0x%08X does not match this build, camera gains not patched.\n", g.va);
                return false;
            }
        }
        const uint8_t dist_expect[4] = { 0xF2, 0x0F, 0x59, 0x0D };
        uint32_t dist_operand = 0;
        memcpy(&dist_operand, (const void*)(kDistSite + 4), 4);
        if (memcmp((const void*)kDistSite, dist_expect, 4) != 0 || dist_operand != kDistConst) {
            SDLOG(0, "CameraFix: distance smoothing at 0x%08X does not match this build, camera gains not patched.\n", kDistSite);
            return false;
        }

        uint8_t* cave = (uint8_t*)VirtualAlloc(nullptr, 0x400, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!cave) return false;
        uint8_t* p = cave;
        DWORD oldProtect;
        const size_t kSiteCount = sizeof(kGainSites) / sizeof(kGainSites[0]);
        uint8_t* stubs[sizeof(kGainSites) / sizeof(kGainSites[0])];

        for (size_t i = 0; i < kSiteCount; ++i) {
            const GainSite& g = kGainSites[i];
            uint8_t* stub = p;
            stubs[i] = stub;
            const size_t size = (g.xmm < 0) ? 6 : 8;

            *p++ = 0x50; // push eax
            *p++ = 0xFF; *p++ = 0xB3; // push dword ptr [ebx+offset]
            memcpy(p, &g.offset, 4); p += 4;

            *p++ = 0xE8; // call gain_thunk
            uint32_t thunk_rel = (uint32_t)&gain_thunk - ((uint32_t)p + 4);
            memcpy(p, &thunk_rel, 4); p += 4;

            if (g.xmm < 0) {
                *p++ = 0xA3; // mov [g_fmul_tmp], eax
                uint32_t tmp_addr = (uint32_t)&g_fmul_tmp;
                memcpy(p, &tmp_addr, 4); p += 4;
                *p++ = 0x58; // pop eax
                *p++ = 0xD8; *p++ = 0x0D; // fmul dword ptr [g_fmul_tmp]
                memcpy(p, &tmp_addr, 4); p += 4;
            }
            else {
                *p++ = 0x66; *p++ = 0x0F; *p++ = 0x6E; *p++ = (uint8_t)(0xC0 | (g.xmm << 3)); // movd xmmN, eax
                *p++ = 0x58; // pop eax
            }

            *p++ = 0xE9; // jmp back
            uint32_t jmp_back_rel = (g.va + size) - ((uint32_t)p + 4);
            memcpy(p, &jmp_back_rel, 4); p += 4;
        }
        FlushInstructionCache(GetCurrentProcess(), cave, 0x400);

        // Redirect the original loads to the generated stubs (stubs are complete before any jump points at them)
        for (size_t i = 0; i < kSiteCount; ++i) {
            const GainSite& g = kGainSites[i];
            const size_t size = (g.xmm < 0) ? 6 : 8;
            uint8_t* stub = stubs[i];
            if (VirtualProtect((LPVOID)g.va, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                uint8_t jump[8] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90 };
                int32_t rel = (int32_t)((uint32_t)stub - (g.va + 5));
                memcpy(jump + 1, &rel, 4);
                memcpy((void*)g.va, jump, size);
                FlushInstructionCache(GetCurrentProcess(), (LPCVOID)g.va, size);
                VirtualProtect((LPVOID)g.va, size, oldProtect, &oldProtect);
            }
        }

        // Patch the isolated double-precision multiplier for camera distance easing
        if (VirtualProtect((LPVOID)(kDistSite + 4), 4, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            uint32_t dist_addr = (uint32_t)&g_dist_k;
            memcpy((void*)(kDistSite + 4), &dist_addr, 4);
            FlushInstructionCache(GetCurrentProcess(), (LPCVOID)kDistSite, 8);
            VirtualProtect((LPVOID)(kDistSite + 4), 4, oldProtect, &oldProtect);
        }
        return true;
    }
}

bool installCameraFix() {
    // 1. The two ChrFollowCam::Update calls must really call it (as in zofrea)
    for (uint32_t site : kCallSites) {
        const uint8_t* p = (const uint8_t*)site;
        int32_t rel = 0;
        memcpy(&rel, p + 1, 4);
        if (p[0] != 0xE8 || (uint32_t)(site + 5 + rel) != kFollowUpdate) {
            SDLOG(0, "CameraFix: call site 0x%08X does not call 0x%08X, camera not patched.\n", site, kFollowUpdate);
            return false;
        }
    }

    DWORD oldProtect;
    for (uint32_t site : kCallSites) {
        if (VirtualProtect((LPVOID)(site + 1), 4, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            int32_t rel = (int32_t)((uint32_t)&follow_stub - (site + 5));
            memcpy((void*)(site + 1), &rel, 4);
            FlushInstructionCache(GetCurrentProcess(), (LPCVOID)site, 5);
            VirtualProtect((LPVOID)(site + 1), 4, oldProtect, &oldProtect);
        }
        else return false;
    }

    // 2. The 16 gain loads and the distance easing (skipped as a whole if this build differs)
    const bool gains = install_gain_sites();

    SDLOG(0, "CameraFix: follow camera smoothing patched (blend weights%s).\n",
        gains ? " + 16 gain loads + distance easing" : " only, gain loads skipped");
    return true;
}
