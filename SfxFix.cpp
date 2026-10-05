#include "SfxFix.h"
#include "main.h" // For SDLOG
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {
    uint32_t kRegisterEffect = 0x00D19CF0;
    const float kMinInterval = 1.0f / 30.0f;

    typedef void* (__stdcall* RegisterFn)(const void* name, uint8_t* data, uint32_t size);
    RegisterFn g_orig = nullptr;

    uint32_t rd32(const uint8_t* d, uint32_t o) {
        uint32_t v;
        std::memcpy(&v, d + o, 4);
        return v;
    }

    // Parses the FXR binary structure to find and patch particle spawn intervals
    int raise_intervals(uint8_t* d, uint32_t size) {
        if (!d || size < 0x20 || std::memcmp(d, "FXR\0", 4) != 0) return 0;

        uint32_t table = rd32(d, 0x0C);
        uint32_t count = rd32(d, 0x10);
        if (table > size || count > (size - table) / 4) return 0;

        std::vector<uint32_t> fields(count);
        for (uint32_t i = 0; i < count; ++i) fields[i] = rd32(d, table + i * 4);
        std::sort(fields.begin(), fields.end());

        auto listed = [&](uint32_t o) { return std::binary_search(fields.begin(), fields.end(), o); };

        int raised = 0;
        for (uint32_t o : fields) {
            if (o < 0x14 || o > size - 4) continue;
            uint32_t ast = o - 0x14;
            uint32_t pond1 = rd32(d, ast), n1 = rd32(d, ast + 4), n2 = rd32(d, ast + 8);
            const uint8_t* flags = d + ast + 12;
            uint32_t pond2 = rd32(d, ast + 16);

            if (n1 != n2 || n1 >= 4096 || flags[3] != 0 || flags[0] > 1 || flags[1] > 1 || flags[2] > 1) continue;
            if ((pond1 && !listed(ast)) || (pond2 && !listed(ast + 16))) continue;

            uint32_t target = rd32(d, o);
            if (target > size - 0x18) continue;

            uint32_t type = rd32(d, target);
            uint32_t at = 0;
            if (type == 2) {
                at = target + 0x10;
            }
            else if (type == 3) {
                at = target + 0x0C;
            }
            else {
                continue;
            }

            float v;
            std::memcpy(&v, d + at, 4);
            // If the interval is below 1/30th of a second, clamp it
            if (v >= 0.0f && v < kMinInterval - 1e-6f) {
                std::memcpy(d + at, &kMinInterval, 4);
                ++raised;
            }
        }
        return raised;
    }

    void* __stdcall hk_register(const void* name, uint8_t* data, uint32_t size) {
        __try {
            raise_intervals(data, size);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            // Failsafe: if parsing fails, gracefully ignore and let the game load the original effect
        }
        return g_orig(name, data, size);
    }
}

bool installSfxFix() {
    BYTE* site = (BYTE*)kRegisterEffect;

    // Safety check: mov eax,[0x013787C8] ; push esi ; push edi
    if (site[0] != 0xA1 || site[1] != 0xC8 || site[2] != 0x87 || site[5] != 0x56) {
        SDLOG(0, "SfxFix: Memory mismatch, skipping particle effect fix.\n");
        return false;
    }

    // Allocate trampoline cave
    size_t kStolen = 5;
    BYTE* tramp = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;

    std::memcpy(tramp, site, kStolen);
    tramp[kStolen] = 0xE9;
    int32_t back = (int32_t)((kRegisterEffect + kStolen) - (uint32_t)(tramp + kStolen + 5));
    std::memcpy(tramp + kStolen + 1, &back, 4);
    FlushInstructionCache(GetCurrentProcess(), tramp, 32);

    g_orig = (RegisterFn)tramp;

    DWORD oldProtect;
    if (VirtualProtect(site, kStolen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        site[0] = 0xE9;
        int32_t rel = (int32_t)((uint32_t)&hk_register - (kRegisterEffect + 5));
        std::memcpy(site + 1, &rel, 4);
        FlushInstructionCache(GetCurrentProcess(), site, kStolen);
        VirtualProtect(site, kStolen, oldProtect, &oldProtect);

        SDLOG(0, "SfxFix: Particle effect spawn intervals successfully patched.\n");
        return true;
    }
    return false;
}