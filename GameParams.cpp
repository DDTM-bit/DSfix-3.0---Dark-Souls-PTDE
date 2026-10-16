#include "GameParams.h"
#include "Settings.h" // SDLOG reads the log level when RELEASE_VER is not defined
#include "main.h"
#include <windows.h>
#include <cstring>

// Game parameter tables in memory (research: the Dark Souls Overhaul team, metal-crow & Sean Pesce, for the table
// headers and layouts; the search and checks are DSfix's own).
//
// A loaded .param file starts with a header: +0x06 two 16-bit values 1, 1 (for these two tables), +0x0A the number of
// rows, +0x0C the table name ("EQUIP_PARAM_GOODS_ST" / "EQUIP_PARAM_WEAPON_ST"), and from +0x30 one 12-byte entry per
// row: { row id, offset of the row from the start of the file, offset of its name }. The rows follow, all the same size.
//
// The tables are found by searching readable memory for the name, once the game has loaded them (a few seconds after
// start-up). A hit counts only if the whole header is consistent: row offsets evenly spaced and inside the memory block,
// ids in increasing order, the expected row size, and known rows present. Anything else is ignored.

namespace {
    struct ParamTable {
        const uint8_t* base;
        uint32_t count;
        uint32_t stride;
    };

    struct RowInfo {
        uint32_t id;
        uint32_t offset;
        uint32_t nameOffset;
    };

    // EquipParamGoods: one row is 0x5C bytes. Byte 0x45 holds the flags isAutoEquip (bit 0), isEstablishment,
    // isOnlyOne, isDrop, isDeposit, isDisableHand, IsTravelItem, isSuppleItem. The Estus Flasks are ids 200-215.
    const char kGoodsName[] = "EQUIP_PARAM_GOODS_ST";
    const uint32_t kGoodsStride = 0x5C;
    const uint32_t kGoodsAutoEquipByte = 0x45;
    const uint8_t kGoodsAutoEquipBit = 0x01;
    const uint32_t kEstusFirst = 200, kEstusLast = 215;

    // EquipParamWeapon: the maximum durability ("durabilityMax", 16 bit) is at +0xBE of a row, the starting durability
    // ("durability") at +0xBC.
    const char kWeaponName[] = "EQUIP_PARAM_WEAPON_ST";
    const uint32_t kWeaponDurability = 0xBC;
    const uint32_t kWeaponDurabilityMax = 0xBE;

    // Published by the search thread once found (the table memory stays valid for the whole session).
    const uint8_t* volatile g_weaponBase = NULL;
    volatile uint32_t g_weaponCount = 0;

    bool readableProtect(DWORD p) {
        if (p & (PAGE_GUARD | PAGE_NOACCESS)) return false;
        return (p & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
            PAGE_EXECUTE_WRITECOPY)) != 0;
    }

    // Checks a candidate header. 'avail' = bytes readable from 'base' to the end of its memory region.
    // wantStride: required row size (0 = any). Called inside __try by the caller.
    bool checkTable(const uint8_t* base, size_t avail, uint32_t wantStride, ParamTable* out) {
        if (avail < 0x30 + 12) return false;
        if (base[6] != 1 || base[7] != 0 || base[8] != 1 || base[9] != 0) return false;
        const uint32_t count = *(const uint16_t*)(base + 0x0A);
        if (count < 16 || count > 20000) return false;
        if (avail < 0x30 + (size_t)count * 12) return false;
        const RowInfo* info = (const RowInfo*)(base + 0x30);
        const uint32_t stride = info[1].offset - info[0].offset;
        if (stride < 16 || stride > 0x1000) return false;
        if (wantStride && stride != wantStride) return false;
        if (info[0].offset < 0x30 + count * 12) return false;
        for (uint32_t i = 1; i < count; ++i) {
            if (info[i].offset - info[i - 1].offset != stride) return false;
            if (info[i].id <= info[i - 1].id) return false;
        }
        if ((size_t)info[count - 1].offset + stride > avail) return false;
        out->base = base;
        out->count = count;
        out->stride = stride;
        return true;
    }

    // Searches one memory region for a table name; fills up to maxOut valid tables. No C++ objects here (__try).
    int searchRegion(const uint8_t* start, size_t size, const char* name, size_t nameLen, uint32_t wantStride,
        ParamTable* out, int maxOut) {
        int found = 0;
        __try {
            if (size < 0x0C + nameLen + 1) return 0;
            const uint8_t* p = start + 0x0C;
            const uint8_t* end = start + size - nameLen - 1;
            while (p < end && found < maxOut) {
                p = (const uint8_t*)memchr(p, name[0], end - p);
                if (!p) break;
                if (memcmp(p, name, nameLen + 1) == 0) { // the name and its terminating 0
                    const uint8_t* base = p - 0x0C;
                    ParamTable t;
                    if (checkTable(base, (size_t)(start + size - base), wantStride, &t)) out[found++] = t;
                }
                ++p;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            // the region was released while being read: whatever was found before stays valid
        }
        return found;
    }

    // Searches all readable, committed, non-image memory. Returns the number of valid tables (up to maxOut).
    int findTables(const char* name, uint32_t wantStride, ParamTable* out, int maxOut) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        const uint8_t* p = (const uint8_t*)si.lpMinimumApplicationAddress;
        const uint8_t* maxAddr = (const uint8_t*)si.lpMaximumApplicationAddress;
        const size_t nameLen = strlen(name);
        int found = 0;
        while (p < maxAddr && found < maxOut) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
            const uint8_t* regionStart = (const uint8_t*)mbi.BaseAddress;
            const uint8_t* regionEnd = regionStart + mbi.RegionSize;
            if (mbi.State == MEM_COMMIT && mbi.Type != MEM_IMAGE && readableProtect(mbi.Protect)) {
                found += searchRegion(regionStart, mbi.RegionSize, name, nameLen, wantStride, out + found, maxOut - found);
            }
            if (regionEnd <= p) break;
            p = regionEnd;
        }
        return found;
    }

    bool hasRow(const ParamTable& t, uint32_t id) {
        const RowInfo* info = (const RowInfo*)(t.base + 0x30);
        for (uint32_t i = 0; i < t.count; ++i) if (info[i].id == id) return true;
        return false;
    }

    // Weapon table: the durability fields must look right in most rows (start <= max, max between 1 and 9999).
    bool weaponLayoutPlausible(const ParamTable& t) {
        if (t.stride < kWeaponDurabilityMax + 2) return false;
        const RowInfo* info = (const RowInfo*)(t.base + 0x30);
        uint32_t good = 0;
        for (uint32_t i = 0; i < t.count; ++i) {
            const uint8_t* row = t.base + info[i].offset;
            const uint16_t start = *(const uint16_t*)(row + kWeaponDurability);
            const uint16_t mx = *(const uint16_t*)(row + kWeaponDurabilityMax);
            if (mx >= 1 && mx <= 9999 && start <= mx) ++good;
        }
        return good * 10 >= t.count * 9;
    }

    // Clears isAutoEquip in every row except the Estus Flasks. Returns the number of rows changed.
    int disableAutoEquip(const ParamTable& t) {
        int changed = 0;
        __try {
            const RowInfo* info = (const RowInfo*)(t.base + 0x30);
            for (uint32_t i = 0; i < t.count; ++i) {
                if (info[i].id >= kEstusFirst && info[i].id <= kEstusLast) continue;
                uint8_t* flags = (uint8_t*)(t.base + info[i].offset + kGoodsAutoEquipByte);
                if (*flags & kGoodsAutoEquipBit) {
                    *flags &= (uint8_t)~kGoodsAutoEquipBit;
                    ++changed;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return -1; // not writable: left as it is
        }
        return changed;
    }

    DWORD WINAPI gameParamsThread(LPVOID) {
        bool needGoods = Settings::get().getDisableItemAutoEquip();
        bool needWeapon = Settings::get().getDurabilityBars();
        Sleep(2000);
        // The tables are loaded during start-up; try every 2 s for 5 minutes, then give up.
        for (int attempt = 0; attempt < 150 && (needGoods || needWeapon); ++attempt) {
            if (needGoods) {
                ParamTable t[8];
                const int n = findTables(kGoodsName, kGoodsStride, t, 8);
                int tables = 0, rows = 0;
                for (int i = 0; i < n; ++i) {
                    if (!hasRow(t[i], kEstusFirst)) continue; // must be the real item table
                    const int c = disableAutoEquip(t[i]);
                    if (c >= 0) { ++tables; rows += c; }
                }
                if (tables > 0) {
                    needGoods = false;
                    SDLOG(0, "disableItemAutoEquip: auto-equip turned off for %d items (%d table copies; Estus unchanged)\n", rows, tables);
                }
            }
            if (needWeapon) {
                ParamTable t[8];
                const int n = findTables(kWeaponName, 0, t, 8);
                for (int i = 0; i < n; ++i) {
                    if (!weaponLayoutPlausible(t[i])) continue;
                    g_weaponCount = t[i].count;
                    g_weaponBase = t[i].base; // published last
                    needWeapon = false;
                    SDLOG(0, "durabilityBars: weapon table found (%u weapons)\n", t[i].count);
                    break;
                }
            }
            if (needGoods || needWeapon) Sleep(2000);
        }
        if (needGoods) SDLOG(0, "disableItemAutoEquip: item table not found, auto-equip unchanged\n");
        if (needWeapon) SDLOG(0, "durabilityBars: weapon table not found, no durability bars\n");
        return 0;
    }
}

void startGameParamsThread() {
    static bool started = false;
    if (started) return;
    if (!Settings::get().getDisableItemAutoEquip() && !Settings::get().getDurabilityBars()) return;
    started = true;
    HANDLE h = CreateThread(NULL, 0, gameParamsThread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}

bool weaponMaxDurability(uint32_t weaponId, uint32_t* maxDurability) {
    const uint8_t* base = g_weaponBase;
    const uint32_t count = g_weaponCount;
    if (!base || count == 0 || !maxDurability) return false;
    // The row of the weapon as it is; if there is none, the row without the upgrade level (the last two digits)
    const RowInfo* info = (const RowInfo*)(base + 0x30);
    const uint32_t ids[2] = { weaponId, weaponId - weaponId % 100 };
    for (int k = 0; k < 2; ++k) {
        uint32_t lo = 0, hi = count;
        while (lo < hi) { // ids are in increasing order (checked when the table was found)
            const uint32_t mid = (lo + hi) / 2;
            if (info[mid].id < ids[k]) lo = mid + 1;
            else hi = mid;
        }
        if (lo < count && info[lo].id == ids[k]) {
            *maxDurability = *(const uint16_t*)(base + info[lo].offset + kWeaponDurabilityMax);
            return *maxDurability > 0;
        }
    }
    return false;
}
