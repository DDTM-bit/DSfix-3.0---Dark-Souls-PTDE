#include "CameraFix.h"
#include "PhysicsFixes.h" // For g_PhysicsDeltaTime
#include "main.h"         // For SDLOG
#include <windows.h>
#include <cmath>
#include <cstring>
#include <cstdint>

namespace {
    uint32_t kFollowUpdate = 0x00F02720;
    uint32_t kCallSites[] = { 0x00F148DD, 0x00F148FE };

    using FollowFn = void(__stdcall*)(void*, float, void*, void*);
    FollowFn g_orig = (FollowFn)0x00F02720;

    struct Field {
        float original = 0.0f;
        float written = -1.0f;
    };

    struct CameraObj {
        void* object = nullptr;
        Field field[2];
    };
    CameraObj g_cameras[4];

    CameraObj* camera_slot(void* object) {
        CameraObj* free_slot = nullptr;
        for (auto& c : g_cameras) {
            if (c.object == object) return &c;
            if (!c.object && !free_slot) free_slot = &c;
        }
        if (!free_slot) free_slot = &g_cameras[0];
        free_slot->object = object;
        return free_slot;
    }

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
        __except (1) { return false; }
    }

    bool write_field(void* object, uint32_t offset, float value) {
        __try {
            *(volatile float*)((uint8_t*)object + offset) = value;
            return true;
        }
        __except (1) { return false; }
    }

    void scale_camera(void* object, float dt) {
        if (!object) return;
        double n = (double)dt * 30.0;
        if (n < 0.02) n = 0.02;
        if (n > 10.0) n = 10.0;

        CameraObj* cam = camera_slot(object);
        uint32_t kFields[] = { 0x238, 0x1BC };
        for (int i = 0; i < 2; ++i) {
            float value = 0.0f;
            if (!read_field(object, kFields[i], &value)) return;
            Field& f = cam->field[i];
            if (value != f.written) {
                f.original = value;
            }
            float target = scaled(f.original, n);
            f.written = target;
            write_field(object, kFields[i], target);
        }
    }

    float g_gain_n = 1.0f;
    float g_fmul_tmp = 0.0f;
    alignas(8) double g_dist_k = 0.1;

    // x87 FPU math block to calculate 1 - (1 - w)^n quickly
    __declspec(naked) void gain_thunk() {
        __asm {
            mov eax, [esp + 4]
            pushfd
            test eax, eax
            jle done
            cmp eax, 0x3F800000 // 1.0f
            jge done
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
        double n = (double)g_PhysicsDeltaTime * 30.0;
        if (n < 0.02) n = 0.02;
        if (n > 10.0) n = 10.0;

        g_gain_n = (float)n;
        g_dist_k = 1.0 - std::pow(1.0 - 0.1, n);

        scale_camera(camera, g_PhysicsDeltaTime);
        g_orig(camera, dt, chr, extra);
    }

    struct GainSite {
        uint32_t va;
        uint32_t offset;
        int xmm; // -1 indicates an fmul instruction
    };

    const GainSite kGainSites[] = {
        {0x00F0B5F3, 0x1A4, 0}, {0x00F0B5FD, 0x1C0, 0}, {0x00F0B61A, 0x1B0, 1}, {0x00F0B624, 0x1C4, 1},
        {0x00F04FA5, 0x190, 0}, {0x00F079D3, 0x1A0, -1}, {0x00F075D2, 0x234, 2}, {0x00F076CA, 0x234, 2},
        {0x00F06EEB, 0x288, 2}, {0x00F06F31, 0x288, 2}, {0x00F02814, 0x320, 2}, {0x00F02859, 0x320, 2},
        {0x00F02892, 0x320, 2}, {0x00F028D1, 0x320, 2}, {0x00F0297B, 0x320, 2},
        {0x00F0C64C, 0x130, 0} //the 16th camera boost site 
    };
}

bool installCameraFix() {
    DWORD oldProtect;

    // 1. Hook the two ChrFollowCam::Update calls
    for (uint32_t site : kCallSites) {
        if (VirtualProtect((LPVOID)(site + 1), 4, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            int32_t rel = (int32_t)((uint32_t)&follow_stub - (site + 5));
            memcpy((void*)(site + 1), &rel, 4);
            VirtualProtect((LPVOID)(site + 1), 4, oldProtect, &oldProtect);
        }
        else return false;
    }

    // 2. Allocate a memory cave to hold our 15 inline stubs
    uint8_t* cave = (uint8_t*)VirtualAlloc(nullptr, 0x400, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!cave) return false;
    uint8_t* p = cave;

    // Generate dynamic machine code for each camera blending site
    for (const GainSite& g : kGainSites) {
        uint8_t* stub = p;
        size_t size = (g.xmm < 0) ? 6 : 8;

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

        // Redirect original instruction to the generated stub
        if (VirtualProtect((LPVOID)g.va, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            uint8_t jump[8] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90 };
            int32_t rel = (int32_t)((uint32_t)stub - (g.va + 5));
            memcpy(jump + 1, &rel, 4);
            memcpy((void*)g.va, jump, size);
            VirtualProtect((LPVOID)g.va, size, oldProtect, &oldProtect);
        }
    }

    // 3. Patch the isolated double-precision multiplier for camera distance easing
    uint32_t kDistSite = 0x00F029B4;
    if (VirtualProtect((LPVOID)(kDistSite + 4), 4, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        uint32_t dist_addr = (uint32_t)&g_dist_k;
        memcpy((void*)(kDistSite + 4), &dist_addr, 4);
        VirtualProtect((LPVOID)(kDistSite + 4), 4, oldProtect, &oldProtect);
    }

    SDLOG(0, "CameraFix: Lock-on panning and smoothing successfully patched.\n");
    return true;
}